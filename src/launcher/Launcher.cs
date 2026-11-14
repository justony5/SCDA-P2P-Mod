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
        ComboBox txtNetwork;
        TextBox txtLog;
        Label lblNode;
        CheckBox chkCloseApp, chkWasd;
        ComboBox cboMode, cboRes;
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
            ClientSize = new Size(520, 512); Font = new Font("Segoe UI", 9f);
            StartPosition = FormStartPosition.CenterScreen;
            try { Icon = Icon.ExtractAssociatedIcon(gameExe); } catch { }

            Controls.Add(new Label { Text = "ZeroTier network ID (16 characters):", Left = 12, Top = 14, AutoSize = true });
            txtNetwork = new ComboBox { Left = 12, Top = 34, Width = 200, DropDownStyle = ComboBoxStyle.DropDown,
                                        Font = new Font("Consolas", 11f), Text = LoadNetworkId() };
            txtNetwork.DropDownWidth = 400;
            // Remember the ID as soon as a valid one is typed or picked, not only when PLAY is pressed.
            txtNetwork.TextChanged += (s, e) => { var id = CleanId(txtNetwork.Text); if (id != null) SaveNetworkId(id); };
            Controls.Add(txtNetwork);

            lblNode = new Label { Text = "Your ZeroTier node ID: (checking...)", Left = 230, Top = 38, AutoSize = true };
            Controls.Add(lblNode);
            btnCopy = new Button { Text = "Copy", Left = 450, Top = 33, Width = 58, Enabled = false };
            btnCopy.Click += (s, e) => { if (nodeId.Length > 0) Clipboard.SetText(nodeId); };
            Controls.Add(btnCopy);

            chkCloseApp = new CheckBox { Text = "Close the ZeroTier app (tray icon) when the game closes", Left = 12, Top = 72, Width = 496,
                                         Checked = Ini.GetBool("Launcher", "CloseZeroTierApp", true) };
            Controls.Add(chkCloseApp);

            var grp = new GroupBox { Text = "Game settings (applied when you press PLAY)", Left = 12, Top = 98, Width = 496, Height = 104 };
            grp.Controls.Add(new Label { Text = "Display:", Left = 10, Top = 26, AutoSize = true });
            cboMode = new ComboBox { Left = 80, Top = 22, Width = 150, DropDownStyle = ComboBoxStyle.DropDownList };
            cboMode.Items.AddRange(new object[] { "Borderless window", "Fullscreen", "Window" });
            grp.Controls.Add(new Label { Text = "Resolution:", Left = 250, Top = 26, AutoSize = true });
            cboRes = new ComboBox { Left = 330, Top = 22, Width = 150, DropDownStyle = ComboBoxStyle.DropDownList };
            chkWasd = new CheckBox { Text = "WASD movement (the game ships with the French Z/Q/S/D layout)", Left = 10, Top = 58, Width = 476,
                                     Checked = Ini.GetBool("Controls", "WASD", true) };
            grp.Controls.AddRange(new Control[] { cboMode, cboRes, chkWasd });
            Controls.Add(grp);
            FillDisplayChoices();

            btnPlay = new Button { Text = "PLAY", Left = 12, Top = 212, Width = 496, Height = 40, Font = new Font("Segoe UI", 12f, FontStyle.Bold) };
            btnPlay.Click += (s, e) => StartPlay();
            Controls.Add(btnPlay);

            txtLog = new TextBox { Left = 12, Top = 262, Width = 496, Height = 216, Multiline = true, ReadOnly = true,
                                   ScrollBars = ScrollBars.Vertical, BackColor = Color.Black, ForeColor = Color.LightGreen,
                                   Font = new Font("Consolas", 8.5f) };
            Controls.Add(txtLog);

            Controls.Add(new Label { Text = "Developed by Deceitful Gaming | Tech  x  Claude Code",
                                     Left = 12, Top = 486, Width = 496, Height = 18, TextAlign = ContentAlignment.MiddleCenter,
                                     ForeColor = Color.DimGray, Font = new Font("Segoe UI", 8.5f, FontStyle.Italic) });

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
            chkCloseApp.Enabled = cboMode.Enabled = cboRes.Enabled = chkWasd.Enabled = !busy;
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
                    FillJoinedNetworks();
                    if (autoPlay) BeginInvoke(new Action(StartPlay));
                }
                catch (Exception ex) { Log("ERROR: " + ex.Message); }
            }) { IsBackground = true }.Start();
        }


        // ---- display / controls choices
        void FillDisplayChoices()
        {
            string mode = Ini.Get("Display", "Mode", "Borderless window");
            cboMode.SelectedItem = cboMode.Items.Contains(mode) ? mode : "Borderless window";
            var native = GameSettings.NativeResolution();
            cboRes.Items.Add("Native (" + native + ")");
            foreach (var r in GameSettings.SupportedResolutions()) cboRes.Items.Add(r);
            string res = Ini.Get("Display", "Resolution", "");
            cboRes.SelectedIndex = (res.Length > 0 && cboRes.Items.Contains(res)) ? cboRes.Items.IndexOf(res) : 0;
        }

        void SaveDisplayChoices()
        {
            Ini.Set("Display", "Mode", Convert.ToString(cboMode.SelectedItem));
            Ini.Set("Display", "Resolution", cboRes.SelectedIndex <= 0 ? "" : Convert.ToString(cboRes.SelectedItem));
            Ini.Set("Controls", "WASD", chkWasd.Checked ? "1" : "0");
        }

        // ---- network ID: saved next to the game and in %APPDATA% as a backup
        static string UserIniPath()
        {
            string dir = Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.ApplicationData), "SCDA_P2P");
            Directory.CreateDirectory(dir);
            return Path.Combine(dir, "launcher.ini");
        }

        // Accepts "0123456789abcdef" or a drop-down entry like "0123456789abcdef  (my network)".
        static string CleanId(string text)
        {
            var m = Regex.Match((text ?? "").Trim().ToLowerInvariant(), @"^([0-9a-f]{16})\b");
            return m.Success ? m.Groups[1].Value : null;
        }

        static string LoadNetworkId()
        {
            string id = Ini.Get("Launcher", "NetworkID", "").Trim();
            if (id.Length == 0)
            {
                string saved = Ini.Path; Ini.Path = UserIniPath();
                try { id = Ini.Get("Launcher", "NetworkID", "").Trim(); } finally { Ini.Path = saved; }
            }
            return id;
        }

        static void SaveNetworkId(string id)
        {
            Ini.Set("Launcher", "NetworkID", id);
            string saved = Ini.Path; Ini.Path = UserIniPath();
            try { Ini.Set("Launcher", "NetworkID", id); } finally { Ini.Path = saved; }
        }

        // Lists networks this PC has already joined in ZeroTier; fills the box if it is empty.
        void FillJoinedNetworks()
        {
            var items = new List<string>();
            try
            {
                var list = zt.Json("listnetworks") as object[];
                if (list != null)
                    foreach (Dictionary<string, object> n in list)
                    {
                        string id = Convert.ToString(n.ContainsKey("nwid") ? n["nwid"] : (n.ContainsKey("id") ? n["id"] : ""));
                        string name = n.ContainsKey("name") ? Convert.ToString(n["name"]) : "";
                        if (Regex.IsMatch(id, "^[0-9a-f]{16}$")) items.Add(name.Length > 0 ? id + "  (" + name + ")" : id);
                    }
            }
            catch { return; }
            if (items.Count == 0) return;
            BeginInvoke(new Action(() =>
            {
                string current = txtNetwork.Text;
                txtNetwork.Items.Clear();
                txtNetwork.Items.AddRange(items.ToArray());
                if (CleanId(current) != null) txtNetwork.Text = current;
                else { txtNetwork.SelectedIndex = 0; Log("Filled in your joined ZeroTier network " + CleanId(txtNetwork.Text) + "."); }
            }));
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
            string nwid = CleanId(txtNetwork.Text);
            if (nwid == null)
            {
                MessageBox.Show(this, "Enter the 16-character ZeroTier network ID (0-9, a-f).", "Network ID", MessageBoxButtons.OK, MessageBoxIcon.Information);
                return;
            }
            SaveNetworkId(nwid);
            Ini.Set("Launcher", "CloseZeroTierApp", chkCloseApp.Checked ? "1" : "0");
            bool closeApp = chkCloseApp.Checked;
            SaveDisplayChoices();

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
                try { GameSettings.Apply(sysDir, Ini.Get("Display", "Mode", "Borderless window"),
                                         Ini.Get("Display", "Resolution", ""), Ini.GetBool("Controls", "WASD", true), Log); }
                catch (Exception ex) { Log("Game settings not applied: " + ex.Message); }
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

    // Writes the game's own settings: Default.ini video section, borderless flag for dinput8.dll,
    // and WASD bindings in the player profiles. Game ini files contain stray non-ASCII bytes, so they
    // are read and written byte-for-byte (ISO-8859-1) and only the needed lines are changed.
    static class GameSettings
    {
        static readonly Encoding Raw = Encoding.GetEncoding(28591);

        [StructLayout(LayoutKind.Sequential, CharSet = CharSet.Unicode)]
        struct DEVMODE
        {
            [MarshalAs(UnmanagedType.ByValTStr, SizeConst = 32)] public string dmDeviceName;
            public short dmSpecVersion, dmDriverVersion, dmSize, dmDriverExtra;
            public int dmFields, dmPositionX, dmPositionY, dmDisplayOrientation, dmDisplayFixedOutput;
            public short dmColor, dmDuplex, dmYResolution, dmTTOption, dmCollate;
            [MarshalAs(UnmanagedType.ByValTStr, SizeConst = 32)] public string dmFormName;
            public short dmLogPixels;
            public int dmBitsPerPel, dmPelsWidth, dmPelsHeight, dmDisplayFlags, dmDisplayFrequency;
            public int dmICMMethod, dmICMIntent, dmMediaType, dmDitherType, dmReserved1, dmReserved2, dmPanningWidth, dmPanningHeight;
        }
        [DllImport("user32.dll", CharSet = CharSet.Unicode)]
        static extern bool EnumDisplaySettings(string dev, int mode, ref DEVMODE dm);

        static DEVMODE NewDm() { var d = new DEVMODE(); d.dmSize = (short)Marshal.SizeOf(typeof(DEVMODE)); return d; }

        // Real pixel size of the main monitor (not affected by Windows display scaling).
        public static string NativeResolution()
        {
            var d = NewDm();
            if (EnumDisplaySettings(null, -1 /* ENUM_CURRENT_SETTINGS */, ref d) && d.dmPelsWidth > 0)
                return d.dmPelsWidth + "x" + d.dmPelsHeight;
            return Screen.PrimaryScreen.Bounds.Width + "x" + Screen.PrimaryScreen.Bounds.Height;
        }

        public static List<string> SupportedResolutions()
        {
            var set = new SortedSet<long>();
            var d = NewDm();
            for (int i = 0; EnumDisplaySettings(null, i, ref d); i++)
                if (d.dmBitsPerPel >= 32 && d.dmPelsWidth >= 1024 && d.dmPelsHeight >= 600)
                    set.Add(((long)d.dmPelsWidth << 16) | (uint)d.dmPelsHeight);
            var list = new List<string>();
            foreach (var v in set.Reverse()) list.Add((v >> 16) + "x" + (v & 0xFFFF));
            return list;
        }

        public static void Apply(string sysDir, string mode, string res, bool wasd, Action<string> log)
        {
            var m = Regex.Match(res ?? "", @"^(\d+)x(\d+)$");
            if (!m.Success) m = Regex.Match(NativeResolution(), @"^(\d+)x(\d+)$");
            string w = m.Groups[1].Value, h = m.Groups[2].Value;
            bool full = mode == "Fullscreen", borderless = mode == "Borderless window";

            string def = Path.Combine(sysDir, "Default.ini");
            if (File.Exists(def))
            {
                string backup = def + ".scdap2p-backup";
                if (!File.Exists(backup)) File.Copy(def, backup);
                string text = File.ReadAllText(def, Raw);
                var sec = Regex.Match(text, @"(?ms)^\[WinDrv\.WindowsClient\]\r?\n.*?(?=^\[|\z)");
                if (sec.Success)
                {
                    string body = sec.Value;
                    var values = new Dictionary<string, string> {
                        { "WindowedViewportX", w }, { "WindowedViewportY", h },
                        { "FullscreenViewportX", w }, { "FullscreenViewportY", h },
                        { "MenuViewportX", w }, { "MenuViewportY", h },
                        { "StartupFullscreen", full ? "True" : "False" } };
                    foreach (var kv in values)
                    {
                        var line = new Regex(@"(?m)^" + kv.Key + @"=[^\r\n]*");
                        body = line.IsMatch(body) ? line.Replace(body, kv.Key + "=" + kv.Value, 1)
                                                  : body.TrimEnd('\r', '\n') + "\r\n" + kv.Key + "=" + kv.Value + "\r\n\r\n";
                    }
                    text = text.Substring(0, sec.Index) + body + text.Substring(sec.Index + sec.Length);
                    File.WriteAllText(def, text, Raw);
                }
            }
            Ini.Set("Display", "Borderless", borderless ? "1" : "0");
            log("Display: " + mode + " " + w + "x" + h);

            int changed = 0;
            var profiles = new List<string> { Path.Combine(sysDir, "PlayerProfilePC.ini") };
            string saves = Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.CommonApplicationData), @"Ubisoft\SplinterCell4\Save\Online");
            if (Directory.Exists(saves)) profiles.AddRange(Directory.GetFiles(saves, "PlayerProfilePC_*.ini"));
            foreach (var f in profiles) if (File.Exists(f) && SetWasd(f, wasd)) changed++;
            if (changed > 0) log((wasd ? "WASD" : "Z/Q/S/D") + " movement set in " + changed + " profile file(s).");
        }

        // Only swaps keys that still have the stock value, so custom bindings are left alone.
        public static bool SetWasd(string file, bool wasd)
        {
            string text = File.ReadAllText(file, Raw), orig = text;
            string[,] map = { { "MoveForward", "IK_Z", "IK_W" }, { "MoveLeft", "IK_Q", "IK_A" }, { "Berzerk", "IK_A", "IK_Q" } };
            for (int i = 0; i < map.GetLength(0); i++)
            {
                string from = wasd ? map[i, 1] : map[i, 2], to = wasd ? map[i, 2] : map[i, 1];
                var rx = new Regex(@"(?m)^(PCBindings=\(ActionName[^=]*=""" + map[i, 0] + @"""[^\r\n]*?Keys[^\[\r\n]*\[0\]="")" + from + @"""");
                text = rx.Replace(text, "${1}" + to + "\"", 1);
            }
            if (text == orig) return false;
            string backup = file + ".scdap2p-backup";
            if (!File.Exists(backup)) File.WriteAllText(backup, orig, Raw);
            File.WriteAllText(file, text, Raw);
            return true;
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
