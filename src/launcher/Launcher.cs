// SCDA P2P Launcher - joins a ZeroTier network, launches Splinter Cell Double Agent
// multiplayer bound to the ZeroTier adapter, and shuts ZeroTier down when the game exits.
// Build: see build.ps1 (uses the .NET Framework 4 csc.exe that ships with Windows).
using System;
using System.Collections;
using System.Collections.Generic;
using System.Diagnostics;
using System.Drawing;
using System.IO;
using System.Linq;
using System.Net.NetworkInformation;
using System.Net.Sockets;
using System.Runtime.InteropServices;
using System.ServiceProcess;
using System.Text;
using System.Text.RegularExpressions;
using System.Threading;
using System.Web.Script.Serialization;
using System.Windows.Forms;

namespace ScdaP2P
{
    static class Ini
    {
        [DllImport("kernel32.dll", CharSet = CharSet.Unicode)]
        static extern int GetPrivateProfileString(string s, string k, string d, StringBuilder r, int n, string f);
        [DllImport("kernel32.dll", CharSet = CharSet.Unicode)]
        static extern bool WritePrivateProfileString(string s, string k, string v, string f);

        public static string Path;
        public static string Get(string sec, string key, string def)
        {
            var sb = new StringBuilder(512);
            GetPrivateProfileString(sec, key, def, sb, sb.Capacity, Path);
            return sb.ToString();
        }
        public static bool GetBool(string sec, string key, bool def)
        {
            return Get(sec, key, def ? "1" : "0").Trim() == "1";
        }
        public static void Set(string sec, string key, string val) { WritePrivateProfileString(sec, key, val, Path); }
    }

    class ZeroTier
    {
        const string ServiceName = "ZeroTierOneService";
        string exe;      // zerotier-one_x64.exe (run with -q) or zerotier-cli.bat
        bool isBat;

        public static ZeroTier Find()
        {
            var pd = Environment.GetFolderPath(Environment.SpecialFolder.CommonApplicationData);
            var pf86 = Environment.GetFolderPath(Environment.SpecialFolder.ProgramFilesX86);
            var pf = Environment.GetFolderPath(Environment.SpecialFolder.ProgramFiles);
            string[] exes = {
                System.IO.Path.Combine(pd, @"ZeroTier\One\zerotier-one_x64.exe"),
                System.IO.Path.Combine(pd, @"ZeroTier\One\zerotier-one_arm64.exe"),
                System.IO.Path.Combine(pd, @"ZeroTier\One\zerotier-one_x86.exe"),
            };
            foreach (var e in exes) if (File.Exists(e)) return new ZeroTier { exe = e };
            string[] bats = {
                System.IO.Path.Combine(pf86, @"ZeroTier\One\zerotier-cli.bat"),
                System.IO.Path.Combine(pf, @"ZeroTier\One\zerotier-cli.bat"),
            };
            foreach (var b in bats) if (File.Exists(b)) return new ZeroTier { exe = b, isBat = true };
            return null;
        }

        public string Run(string args, out int code)
        {
            var psi = new ProcessStartInfo
            {
                FileName = isBat ? "cmd.exe" : exe,
                Arguments = isBat ? "/c \"\"" + exe + "\" " + args + "\"" : "-q " + args,
                UseShellExecute = false, CreateNoWindow = true,
                RedirectStandardOutput = true, RedirectStandardError = true,
            };
            using (var p = Process.Start(psi))
            {
                string o = p.StandardOutput.ReadToEnd() + p.StandardError.ReadToEnd();
                p.WaitForExit(15000);
                code = p.HasExited ? p.ExitCode : -1;
                return o.Trim();
            }
        }

        public object Json(string args)
        {
            int code;
            var o = Run("-j " + args, out code);
            try { return new JavaScriptSerializer().DeserializeObject(o); }
            catch { throw new Exception("ZeroTier returned: " + o); }
        }

        public ServiceController Service()
        {
            return ServiceController.GetServices().FirstOrDefault(s => s.ServiceName == ServiceName);
        }

        public string NodeId()
        {
            var info = Json("info") as Dictionary<string, object>;
            return info != null && info.ContainsKey("address") ? Convert.ToString(info["address"]) : "?";
        }

        public Dictionary<string, object> Network(string nwid)
        {
            var list = Json("listnetworks") as object[];
            if (list == null) return null;
            foreach (Dictionary<string, object> n in list)
            {
                var id = n.ContainsKey("nwid") ? n["nwid"] : (n.ContainsKey("id") ? n["id"] : null);
                if (string.Equals(Convert.ToString(id), nwid, StringComparison.OrdinalIgnoreCase)) return n;
            }
            return null;
        }
    }

    class MainForm : Form
    {
        TextBox txtNetwork, txtLog;
        Label lblNode;
        CheckBox chkCloseApp;
        Button btnPlay, btnCopy;
        string sysDir, gameExe;
        ZeroTier zt;
        string nodeId = "";
        bool autoPlay;

        public MainForm(bool autoPlay)
        {
            this.autoPlay = autoPlay;
            sysDir = AppDomain.CurrentDomain.BaseDirectory;
            Ini.Path = Path.Combine(sysDir, "SCDA_P2P.ini");
            gameExe = Path.Combine(sysDir, Ini.Get("Launcher", "GameExe", "SCDA_online.exe"));

            Text = "Splinter Cell Double Agent - P2P Launcher";
            FormBorderStyle = FormBorderStyle.FixedDialog; MaximizeBox = false;
            ClientSize = new Size(520, 400); Font = new Font("Segoe UI", 9f);
            StartPosition = FormStartPosition.CenterScreen;
            try { Icon = Icon.ExtractAssociatedIcon(gameExe); } catch { }

            Controls.Add(new Label { Text = "ZeroTier network ID (16 characters):", Left = 12, Top = 14, AutoSize = true });
            txtNetwork = new TextBox { Left = 12, Top = 34, Width = 200, MaxLength = 16, CharacterCasing = CharacterCasing.Lower,
                                       Font = new Font("Consolas", 11f), Text = Ini.Get("Launcher", "NetworkID", "") };
            Controls.Add(txtNetwork);

            lblNode = new Label { Text = "Your ZeroTier node ID: (checking...)", Left = 230, Top = 38, AutoSize = true };
            Controls.Add(lblNode);
            btnCopy = new Button { Text = "Copy", Left = 450, Top = 33, Width = 58, Enabled = false };
            btnCopy.Click += (s, e) => { if (nodeId.Length > 0) Clipboard.SetText(nodeId); };
            Controls.Add(btnCopy);

            chkCloseApp = new CheckBox { Text = "Close the ZeroTier app (tray icon) when the game closes", Left = 12, Top = 72, Width = 496,
                                         Checked = Ini.GetBool("Launcher", "CloseZeroTierApp", true) };
            Controls.Add(chkCloseApp);

            btnPlay = new Button { Text = "PLAY", Left = 12, Top = 102, Width = 496, Height = 40, Font = new Font("Segoe UI", 12f, FontStyle.Bold) };
            btnPlay.Click += (s, e) => StartPlay();
            Controls.Add(btnPlay);

            txtLog = new TextBox { Left = 12, Top = 152, Width = 496, Height = 236, Multiline = true, ReadOnly = true,
                                   ScrollBars = ScrollBars.Vertical, BackColor = Color.Black, ForeColor = Color.LightGreen,
                                   Font = new Font("Consolas", 8.5f) };
            Controls.Add(txtLog);

            Shown += (s, e) => OnShownInit();
        }

        void Log(string msg)
        {
            if (InvokeRequired) { BeginInvoke(new Action<string>(Log), msg); return; }
            txtLog.AppendText("[" + DateTime.Now.ToString("HH:mm:ss") + "] " + msg + Environment.NewLine);
        }

        void SetBusy(bool busy)
        {
            if (InvokeRequired) { BeginInvoke(new Action<bool>(SetBusy), busy); return; }
            btnPlay.Enabled = !busy; txtNetwork.Enabled = !busy;
            chkCloseApp.Enabled = !busy;
            if (busy) WindowState = FormWindowState.Minimized; else { WindowState = FormWindowState.Normal; Activate(); }
        }

        void OnShownInit()
        {
            if (!File.Exists(gameExe))
                Log("WARNING: " + Path.GetFileName(gameExe) + " not found. Put this launcher in SCDA-Online\\System.");
            if (!File.Exists(Path.Combine(sysDir, "dinput8.dll")))
                Log("WARNING: dinput8.dll (adapter fix) is missing next to the game. LAN games may not be found.");

            zt = ZeroTier.Find();
            if (zt == null)
            {
                lblNode.Text = "ZeroTier is not installed";
                Log("ZeroTier One is not installed.");
                if (MessageBox.Show(this, "ZeroTier One is required and is not installed.\n\nOpen the ZeroTier download page now?\n" +
                    "Install it, then start this launcher again.", "ZeroTier missing", MessageBoxButtons.YesNo, MessageBoxIcon.Warning) == DialogResult.Yes)
                    Process.Start("https://www.zerotier.com/download/");
                btnPlay.Enabled = false;
                return;
            }
            new Thread(() =>
            {
                try
                {
                    EnsureServiceRunning();
                    nodeId = zt.NodeId();
                    BeginInvoke(new Action(() => { lblNode.Text = "Your node ID: " + nodeId; btnCopy.Enabled = true; }));
                    Log("ZeroTier node ID: " + nodeId + " (the network admin authorizes this ID).");
                    if (autoPlay) BeginInvoke(new Action(StartPlay));
                }
                catch (Exception ex) { Log("ERROR: " + ex.Message); }
            }) { IsBackground = true }.Start();
        }


        void EnsureServiceRunning()
        {
            var svc = zt.Service();
            if (svc == null) throw new Exception("ZeroTier service not found. Reinstall ZeroTier One.");
            svc.Refresh();
            if (svc.Status == ServiceControllerStatus.Running) return;
            Log("Starting ZeroTier service...");
            // Older launcher versions could leave the service on manual start; put it back to automatic.
            if (svc.StartType != ServiceStartMode.Automatic) SetStartMode("auto");
            svc.Start();
            svc.WaitForStatus(ServiceControllerStatus.Running, TimeSpan.FromSeconds(30));
            // give the daemon a moment to open its control port
            for (int i = 0; i < 20; i++)
            {
                int code; zt.Run("info", out code);
                if (code == 0) break;
                Thread.Sleep(500);
            }
        }

        static void SetStartMode(string mode)
        {
            var p = Process.Start(new ProcessStartInfo("sc.exe", "config ZeroTierOneService start= " + mode)
                                  { UseShellExecute = false, CreateNoWindow = true });
            p.WaitForExit(10000);
        }

        void StartPlay()
        {
            string nwid = txtNetwork.Text.Trim();
            if (!Regex.IsMatch(nwid, "^[0-9a-f]{16}$"))
            {
                MessageBox.Show(this, "Enter the 16-character ZeroTier network ID (0-9, a-f).", "Network ID", MessageBoxButtons.OK, MessageBoxIcon.Information);
                return;
            }
            Ini.Set("Launcher", "NetworkID", nwid);
            Ini.Set("Launcher", "CloseZeroTierApp", chkCloseApp.Checked ? "1" : "0");
            bool closeApp = chkCloseApp.Checked;

            btnPlay.Enabled = false;
            new Thread(() => PlayFlow(nwid, closeApp)) { IsBackground = true }.Start();
        }

        // Closes only the ZeroTier desktop/tray app. The ZeroTier service is never stopped or
        // reconfigured, so ZeroTier keeps working normally for anything else.
        void CloseZeroTierApp()
        {
            int n = 0;
            foreach (var p in Process.GetProcesses().Where(p => p.ProcessName.StartsWith("zerotier_desktop_ui", StringComparison.OrdinalIgnoreCase)
                                                             || p.ProcessName.Equals("ZeroTier One", StringComparison.OrdinalIgnoreCase)))
            {
                try { p.Kill(); p.WaitForExit(5000); n++; } catch { }
            }
            if (n > 0) Log("Closed the ZeroTier app.");
        }

        void PlayFlow(string nwid, bool closeApp)
        {
            bool gameStarted = false, crashed = false;
            try
            {
                EnsureServiceRunning();
                EnsureFirewallRule();

                Log("Joining network " + nwid + "...");
                int code; string o = zt.Run("join " + nwid, out code);
                if (code != 0) throw new Exception("join failed: " + o);

                string ip = WaitForAddress(nwid);
                if (ip == null) return;

                Ini.Set("P2P", "PreferredIP", ip);
                Log("Game will use ZeroTier address " + ip);

                if (!File.Exists(gameExe)) throw new Exception("Game not found: " + gameExe);
                Log("Launching " + Path.GetFileName(gameExe) + "... (choose LAN in the multiplayer menu)");
                SetBusy(true);
                gameStarted = true;
                int exitCode;
                using (var game = Process.Start(new ProcessStartInfo(gameExe) { WorkingDirectory = sysDir, UseShellExecute = false }))
                {
                    game.WaitForExit();
                    exitCode = game.ExitCode;
                }
                WaitForAllGameProcesses();
                // Negative codes are Windows exception codes (0x8xxxxxxx / 0xCxxxxxxx) = crash.
                crashed = exitCode < 0;
                Log(crashed ? string.Format("Game crashed (code 0x{0:X8}). See SCDA_P2P.log next to the game.", exitCode)
                            : "Game closed.");
            }
            catch (Exception ex)
            {
                Log("ERROR: " + ex.Message);
            }
            finally
            {
                try { if (gameStarted && closeApp) CloseZeroTierApp(); }
                catch (Exception ex) { Log("Cleanup warning: " + ex.Message); }
                // Close the launcher with the game; stay open if something went wrong so the log can be read.
                if (gameStarted && !crashed) BeginInvoke(new Action(Close));
                else SetBusy(false);
            }
        }

        string WaitForAddress(string nwid)
        {
            DateTime deadline = DateTime.Now.AddSeconds(90);
            string lastStatus = "";
            while (DateTime.Now < deadline)
            {
                var n = zt.Network(nwid);
                if (n != null)
                {
                    string status = Convert.ToString(n.ContainsKey("status") ? n["status"] : "");
                    bool changed = status != lastStatus;
                    if (changed) { Log("Network status: " + status); lastStatus = status; }
                    if (status == "ACCESS_DENIED")
                    {
                        if (changed)
                        {
                            Log("Not authorized yet. Ask the network admin to authorize node " + nodeId + ". Waiting up to 5 minutes...");
                            deadline = DateTime.Now.AddMinutes(5);
                        }
                    }
                    else if (status == "NOT_FOUND")
                    {
                        Log("Network " + nwid + " does not exist. Check the ID.");
                        return null;
                    }
                    else if (status == "OK" && n.ContainsKey("assignedAddresses"))
                    {
                        foreach (var a in (object[])n["assignedAddresses"])
                        {
                            string ip = Convert.ToString(a).Split('/')[0];
                            if (ip.Contains(".") && WindowsHasAddress(ip)) return ip;
                        }
                        if (((object[])n["assignedAddresses"]).Length == 0 && lastStatus == "OK")
                            Log("Connected but no IPv4 address assigned - enable IPv4 auto-assign on the network.");
                    }
                }
                Thread.Sleep(1000);
            }
            Log("Timed out waiting for the ZeroTier network to come up.");
            return null;
        }

        static bool WindowsHasAddress(string ip)
        {
            foreach (var nic in NetworkInterface.GetAllNetworkInterfaces())
                foreach (var u in nic.GetIPProperties().UnicastAddresses)
                    if (u.Address.AddressFamily == AddressFamily.InterNetwork && u.Address.ToString() == ip) return true;
            return false;
        }

        void EnsureFirewallRule()
        {
            // One rule per game folder, so a second copy of the game doesn't replace this one's rule.
            string rule = "SCDA P2P - " + gameExe;
            if (Ini.Get("Launcher", "FirewallRule", "") == rule) return;
            Log("Adding Windows Firewall rule for the game...");
            RunHidden("netsh", "advfirewall firewall delete rule name=\"" + rule + "\"");
            RunHidden("netsh", "advfirewall firewall add rule name=\"" + rule + "\" dir=in action=allow program=\"" + gameExe + "\" enable=yes profile=any");
            Ini.Set("Launcher", "FirewallRule", rule);
        }

        static void RunHidden(string file, string args)
        {
            using (var p = Process.Start(new ProcessStartInfo(file, args) { UseShellExecute = false, CreateNoWindow = true }))
                p.WaitForExit(15000);
        }

        void WaitForAllGameProcesses()
        {
            string name = Path.GetFileNameWithoutExtension(gameExe);
            int quiet = 0;
            while (quiet < 3)
            {
                Thread.Sleep(1000);
                quiet = Process.GetProcessesByName(name).Length == 0 ? quiet + 1 : 0;
            }
        }
    }

    static class Program
    {
        [STAThread]
        static void Main(string[] args)
        {
            Application.EnableVisualStyles();
            bool auto = args.Any(a => a.Equals("--play", StringComparison.OrdinalIgnoreCase) || a.Equals("/play", StringComparison.OrdinalIgnoreCase));
            Application.Run(new MainForm(auto));
        }
    }
}
