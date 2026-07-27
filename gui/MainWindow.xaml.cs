using Microsoft.Win32;
using System.Collections.ObjectModel;
using System.Diagnostics;
using System.Globalization;
using System.IO;
using System.Reflection;
using System.Text.Json;
using System.Text.Json.Serialization;
using System.Windows;
using System.Windows.Controls;
using System.Windows.Input;
using System.Windows.Media;
using System.Windows.Threading;
using WinForms = System.Windows.Forms;
using Brush = System.Windows.Media.Brush;
using Button = System.Windows.Controls.Button;
using MessageBox = System.Windows.MessageBox;
using OpenFileDialog = Microsoft.Win32.OpenFileDialog;
using SaveFileDialog = Microsoft.Win32.SaveFileDialog;

namespace WinAnanicyGui;

public partial class MainWindow : Window
{
    private const string EngineStopEventName = @"Local\WinAnanicy.Engine.Stop";
    private const string UiExitEventName = @"Local\WinAnanicy.UI.Exit";
    private const string StartupRegistryPath = @"Software\Microsoft\Windows\CurrentVersion\Run";
    private const string StartupRegistryValue = "WinAnanicyEngine";

    private readonly string _dataDirectory;
    private readonly string _rulesPath;
    private readonly string _settingsPath;
    private readonly string _statusPath;
    private readonly string _logPath;
    private readonly string _preferencesPath;
    private readonly string _backupsDirectory;
    private readonly ObservableCollection<ProcessRule> _rules = [];
    private readonly List<ProcessItem> _allProcesses = [];
    private readonly Dictionary<int, (TimeSpan Cpu, DateTimeOffset Captured)> _cpuSamples = [];
    private readonly SemaphoreSlim _processRefreshGate = new(1, 1);
    private readonly DispatcherTimer _statusTimer;
    private readonly DispatcherTimer _processTimer;
    private readonly JsonSerializerOptions _jsonOptions = new()
    {
        WriteIndented = true,
        PropertyNameCaseInsensitive = true,
        DefaultIgnoreCondition = JsonIgnoreCondition.WhenWritingNull
    };
    private readonly EventWaitHandle _uiExitEvent;
    private readonly RegisteredWaitHandle _uiExitRegistration;

    private UiPreferences _preferences;
    private EngineSettings _engineSettings = new();
    private EngineStatus? _lastStatus;
    private WinForms.NotifyIcon? _notifyIcon;
    private int _editingRuleIndex = -1;
    private bool _changingLanguage;
    private bool _exitRequested;
    private bool _initialized;

    public MainWindow()
    {
        InitializeComponent();

        _dataDirectory = Path.Combine(
            Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData),
            "WinAnanicy");
        _rulesPath = Path.Combine(_dataDirectory, "rules.json");
        _settingsPath = Path.Combine(_dataDirectory, "settings.json");
        _statusPath = Path.Combine(_dataDirectory, "state.json");
        _logPath = Path.Combine(_dataDirectory, "logs", "win-ananicy.log");
        _preferencesPath = Path.Combine(_dataDirectory, "ui-settings.json");
        _backupsDirectory = Path.Combine(_dataDirectory, "backups");

        _preferences = LoadPreferences();
        _preferences.StartWithWindows = IsStartupRegistered();
        ApplyLanguage(_preferences.Language);
        RulesDataGrid.ItemsSource = _rules;
        FooterPathText.Text = @"%LOCALAPPDATA%\WinAnanicy";

        _uiExitEvent = new EventWaitHandle(
            initialState: false,
            EventResetMode.AutoReset,
            UiExitEventName);
        _uiExitRegistration = ThreadPool.RegisterWaitForSingleObject(
            _uiExitEvent,
            (_, _) => Dispatcher.BeginInvoke(() =>
            {
                _exitRequested = true;
                Close();
            }),
            state: null,
            millisecondsTimeOutInterval: Timeout.Infinite,
            executeOnlyOnce: false);

        _statusTimer = new DispatcherTimer { Interval = TimeSpan.FromSeconds(2) };
        _statusTimer.Tick += async (_, _) =>
        {
            await RefreshEngineStatusAsync();
            if (ActivityPage.Visibility == Visibility.Visible)
            {
                await RefreshLogAsync();
            }
        };
        _processTimer = new DispatcherTimer { Interval = TimeSpan.FromSeconds(5) };
        _processTimer.Tick += async (_, _) =>
        {
            if (ProcessesPage.Visibility == Visibility.Visible)
            {
                await RefreshProcessesAsync();
            }
        };

        Loaded += MainWindow_Loaded;
        StateChanged += (_, _) =>
        {
            if (WindowState == WindowState.Minimized && _preferences.MinimizeToTray)
            {
                HideToTray();
            }
        };
        Closed += (_, _) =>
        {
            _uiExitRegistration.Unregister(null);
            _uiExitEvent.Dispose();
            _notifyIcon?.Dispose();
            System.Windows.Application.Current.Shutdown();
        };
    }

    private async void MainWindow_Loaded(object sender, RoutedEventArgs e)
    {
        try
        {
            EnsureDataFiles();
            await LoadRulesAsync();
            await LoadEngineSettingsAsync();
            ConfigureNotificationIcon();
            await RefreshEngineStatusAsync();
            await RefreshProcessesAsync();
            await RefreshLogAsync();

            if (_preferences.StartWithWindows && !IsEngineHealthy(_lastStatus))
            {
                await StartEngineAsync(showErrors: false);
            }

            _statusTimer.Start();
            _processTimer.Start();
            _initialized = true;
        }
        catch (Exception exception)
        {
            ShowError(exception.Message);
        }
    }

    private UiPreferences LoadPreferences()
    {
        try
        {
            if (File.Exists(_preferencesPath))
            {
                return JsonSerializer.Deserialize<UiPreferences>(
                           File.ReadAllText(_preferencesPath),
                           _jsonOptions) ??
                       new UiPreferences();
            }
        }
        catch
        {
            // Safe defaults are used if the optional UI preferences are damaged.
        }

        return new UiPreferences
        {
            Language = CultureInfo.CurrentUICulture.TwoLetterISOLanguageName == "tr"
                ? "tr"
                : "en"
        };
    }

    private void EnsureDataFiles()
    {
        Directory.CreateDirectory(_dataDirectory);
        Directory.CreateDirectory(Path.GetDirectoryName(_logPath)!);
        Directory.CreateDirectory(_backupsDirectory);

        if (!File.Exists(_rulesPath))
        {
            var bundledRules = Path.Combine(AppContext.BaseDirectory, "data", "rules.json");
            if (File.Exists(bundledRules))
            {
                File.Copy(bundledRules, _rulesPath, overwrite: false);
            }
            else
            {
                File.WriteAllText(_rulesPath, "[]");
            }
        }

        if (!File.Exists(_settingsPath))
        {
            var bundledSettings = Path.Combine(AppContext.BaseDirectory, "data", "settings.json");
            if (File.Exists(bundledSettings))
            {
                File.Copy(bundledSettings, _settingsPath, overwrite: false);
            }
            else
            {
                File.WriteAllText(
                    _settingsPath,
                    JsonSerializer.Serialize(new EngineSettings(), _jsonOptions));
            }
        }
    }

    private async Task LoadRulesAsync()
    {
        var json = await File.ReadAllTextAsync(_rulesPath);
        var loaded = JsonSerializer.Deserialize<List<ProcessRule>>(json, _jsonOptions) ?? [];
        var validation = ValidateRuleSet(loaded);
        if (validation is not null)
        {
            throw new InvalidDataException(validation);
        }

        _rules.Clear();
        foreach (var rule in loaded.OrderBy(rule => rule.ProcessName, StringComparer.OrdinalIgnoreCase))
        {
            _rules.Add(rule);
        }
        DashboardRulesCount.Text = _rules.Count.ToString(CultureInfo.CurrentCulture);
    }

    private async Task LoadEngineSettingsAsync()
    {
        var json = await File.ReadAllTextAsync(_settingsPath);
        _engineSettings =
            JsonSerializer.Deserialize<EngineSettings>(json, _jsonOptions) ??
            new EngineSettings();
        PopulateSettingsControls();
    }

    private void PopulateSettingsControls()
    {
        PowerPlanCheckBox.IsChecked = _engineSettings.PowerPlanEnabled;
        PollIntervalTextBox.Text = _engineSettings.PollIntervalMs.ToString(CultureInfo.InvariantCulture);
        TrimCooldownTextBox.Text = _engineSettings.SmartTrimCooldownSecs.ToString(CultureInfo.InvariantCulture);
        WatchdogRetriesTextBox.Text = _engineSettings.WatchdogMaxRetries.ToString(CultureInfo.InvariantCulture);
        StartupCheckBox.IsChecked = _preferences.StartWithWindows;
        MinimizeTrayCheckBox.IsChecked = _preferences.MinimizeToTray;
    }

    private async Task SaveRulesAsync()
    {
        var validation = ValidateRuleSet(_rules);
        if (validation is not null)
        {
            throw new InvalidDataException(validation);
        }
        await SaveJsonAtomicAsync(_rulesPath, _rules.ToList(), createBackup: true);
        DashboardRulesCount.Text = _rules.Count.ToString(CultureInfo.CurrentCulture);
        RulesDataGrid.Items.Refresh();
        await RefreshProcessesAsync();
    }

    private async Task SaveJsonAtomicAsync<T>(string path, T value, bool createBackup)
    {
        Directory.CreateDirectory(Path.GetDirectoryName(path)!);
        var temporary = path + ".tmp";
        var json = JsonSerializer.Serialize(value, _jsonOptions);
        await File.WriteAllTextAsync(temporary, json);

        if (createBackup && File.Exists(path))
        {
            Directory.CreateDirectory(_backupsDirectory);
            var backupName =
                $"{Path.GetFileNameWithoutExtension(path)}-{DateTime.UtcNow:yyyyMMdd-HHmmssfff}{Path.GetExtension(path)}";
            File.Copy(path, Path.Combine(_backupsDirectory, backupName), overwrite: false);
            TrimBackups(Path.GetFileNameWithoutExtension(path));
        }

        File.Move(temporary, path, overwrite: true);
    }

    private void TrimBackups(string prefix)
    {
        foreach (var oldBackup in new DirectoryInfo(_backupsDirectory)
                     .EnumerateFiles($"{prefix}-*.json")
                     .OrderByDescending(file => file.CreationTimeUtc)
                     .Skip(10))
        {
            oldBackup.Delete();
        }
    }

    private async Task RefreshEngineStatusAsync()
    {
        EngineStatus? status = null;
        try
        {
            if (File.Exists(_statusPath))
            {
                await using var stream = new FileStream(
                    _statusPath,
                    FileMode.Open,
                    FileAccess.Read,
                    FileShare.ReadWrite | FileShare.Delete);
                status = await JsonSerializer.DeserializeAsync<EngineStatus>(stream, _jsonOptions);
            }
        }
        catch (JsonException)
        {
            // Atomic replacement can briefly expose no status; retry on the next tick.
        }
        catch (IOException)
        {
            // The engine is replacing the status document.
        }

        _lastStatus = status;
        var healthy = IsEngineHealthy(status);
        SidebarStatusDot.Fill = healthy
            ? (Brush)FindResource("SuccessBrush")
            : (Brush)FindResource("DangerBrush");
        SidebarStatusText.Text = LocalizationService.Text(healthy ? "Running" : "Stopped");
        SidebarStatusDetail.Text = healthy
            ? $"PID {status!.Pid} · v{status.Version}"
            : LocalizationService.Text("StatusReady");
        DashboardEngineStatus.Text = LocalizationService.Text(healthy ? "Running" : "Stopped");
        DashboardEngineStatus.Foreground = healthy
            ? (Brush)FindResource("SuccessBrush")
            : (Brush)FindResource("TextBrush");
        DashboardMatchedCount.Text = healthy
            ? status!.MatchedProcesses.ToString(CultureInfo.CurrentCulture)
            : "0";
        DashboardAppliedCount.Text = healthy
            ? status!.AppliedProcesses.ToString(CultureInfo.CurrentCulture)
            : "0";
        DashboardRulesCount.Text = _rules.Count.ToString(CultureInfo.CurrentCulture);
        StartEngineButton.IsEnabled = !healthy;
        StopEngineButton.IsEnabled = healthy;
        RestartEngineButton.IsEnabled = healthy;
    }

    private static bool IsEngineHealthy(EngineStatus? status)
    {
        if (status is null || !status.Running || status.Pid <= 0)
        {
            return false;
        }
        if (DateTimeOffset.UtcNow - status.LastCycleUtc > TimeSpan.FromSeconds(8))
        {
            return false;
        }
        try
        {
            using var process = Process.GetProcessById(status.Pid);
            return !process.HasExited;
        }
        catch
        {
            return false;
        }
    }

    private async Task RefreshProcessesAsync()
    {
        if (!await _processRefreshGate.WaitAsync(0))
        {
            return;
        }

        try
        {
            var ruleNames = _rules
                .Select(rule => rule.ProcessName)
                .ToHashSet(StringComparer.OrdinalIgnoreCase);
            var configuredText = LocalizationService.Text("RuleDetected");
            var noRuleText = LocalizationService.Text("NoRule");
            var capturedAt = DateTimeOffset.UtcNow;

            var items = await Task.Run(() =>
            {
                var collected = new List<ProcessItem>();
                var currentSamples = new Dictionary<int, (TimeSpan Cpu, DateTimeOffset Captured)>();
                foreach (var process in Process.GetProcesses())
                {
                    using (process)
                    {
                        try
                        {
                            var name = process.ProcessName.EndsWith(
                                ".exe",
                                StringComparison.OrdinalIgnoreCase)
                                ? process.ProcessName
                                : process.ProcessName + ".exe";
                            var totalCpu = process.TotalProcessorTime;
                            var memoryMb = process.WorkingSet64 / (1024L * 1024L);
                            var cpu = 0.0;
                            if (_cpuSamples.TryGetValue(process.Id, out var previous))
                            {
                                var elapsed = (capturedAt - previous.Captured).TotalMilliseconds;
                                if (elapsed > 0)
                                {
                                    cpu = (totalCpu - previous.Cpu).TotalMilliseconds /
                                          elapsed /
                                          Environment.ProcessorCount *
                                          100.0;
                                    cpu = Math.Clamp(cpu, 0.0, 100.0);
                                }
                            }
                            currentSamples[process.Id] = (totalCpu, capturedAt);
                            var hasRule = ruleNames.Contains(name);
                            collected.Add(new ProcessItem
                            {
                                Name = name,
                                Id = process.Id,
                                CpuPercent = cpu,
                                MemoryMb = memoryMb,
                                HasRule = hasRule,
                                RuleState = hasRule ? configuredText : noRuleText
                            });
                        }
                        catch
                        {
                            // Protected or exiting processes are expected on Windows.
                        }
                    }
                }

                _cpuSamples.Clear();
                foreach (var sample in currentSamples)
                {
                    _cpuSamples[sample.Key] = sample.Value;
                }
                return collected
                    .OrderByDescending(item => item.HasRule)
                    .ThenBy(item => item.Name, StringComparer.OrdinalIgnoreCase)
                    .ThenBy(item => item.Id)
                    .ToList();
            });

            _allProcesses.Clear();
            _allProcesses.AddRange(items);
            ApplyProcessFilter();
        }
        finally
        {
            _processRefreshGate.Release();
        }
    }

    private void ApplyProcessFilter()
    {
        var search = ProcessSearchTextBox.Text.Trim();
        ProcessesDataGrid.ItemsSource = string.IsNullOrWhiteSpace(search)
            ? _allProcesses
            : _allProcesses
                .Where(item => item.Name.Contains(search, StringComparison.OrdinalIgnoreCase))
                .ToList();
    }

    private string? ValidateRuleSet(IEnumerable<ProcessRule> rules)
    {
        var names = new HashSet<string>(StringComparer.OrdinalIgnoreCase);
        foreach (var rule in rules)
        {
            var error = ValidateRule(rule);
            if (error is not null)
            {
                return $"{rule.ProcessName}: {error}";
            }
            if (!names.Add(rule.ProcessName))
            {
                return Bi(
                    $"A duplicate rule exists for {rule.ProcessName}.",
                    $"{rule.ProcessName} için yinelenen bir kural var.");
            }
        }
        return null;
    }

    private string? ValidateRule(ProcessRule rule)
    {
        if (string.IsNullOrWhiteSpace(rule.ProcessName) ||
            !rule.ProcessName.EndsWith(".exe", StringComparison.OrdinalIgnoreCase) ||
            Path.GetFileName(rule.ProcessName) != rule.ProcessName)
        {
            return Bi(
                "Enter an executable file name such as game.exe, not a path.",
                "Yol yerine game.exe gibi bir çalıştırılabilir dosya adı girin.");
        }
        if (rule.CpuLimit is < 0 or > 99)
        {
            return Bi(
                "CPU limit must be 0 (off) or between 1 and 99.",
                "CPU sınırı 0 (kapalı) veya 1 ile 99 arasında olmalıdır.");
        }
        if (rule.SmartTrimThresholdMb is < 32 or > 1048576)
        {
            return Bi(
                "SmartTrim must be between 32 MB and 1 TB.",
                "SmartTrim 32 MB ile 1 TB arasında olmalıdır.");
        }
        if (rule.CpuThrottleTriggerPct.HasValue != rule.CpuThrottleDurationSecs.HasValue)
        {
            return Bi(
                "High-CPU trigger and duration must be filled together.",
                "Yüksek CPU tetikleyicisi ve süresi birlikte doldurulmalıdır.");
        }
        if (rule.CpuThrottleTriggerPct is < 1 or > 100 ||
            rule.CpuThrottleDurationSecs is < 1 or > 3600)
        {
            return Bi(
                "CPU trigger must be 1–100% and duration 1–3600 seconds.",
                "CPU tetikleyicisi %1–100, süre 1–3600 saniye olmalıdır.");
        }
        if (rule.InstanceBalance && rule.CpuThrottleTriggerPct.HasValue)
        {
            return Bi(
                "Instance balancing and high-CPU throttling cannot be enabled together.",
                "Örnek dengeleme ile yüksek CPU sınırlaması birlikte açılamaz.");
        }
        if (rule.Disallowed && rule.KeepAlive)
        {
            return Bi(
                "A process cannot be both blocked and kept alive.",
                "Bir süreç aynı anda hem engellenip hem canlı tutulamaz.");
        }
        if (rule.KeepAlive &&
            (!Path.IsPathFullyQualified(rule.ExecutablePath) ||
             !rule.ExecutablePath.EndsWith(".exe", StringComparison.OrdinalIgnoreCase)))
        {
            return Bi(
                "Keep Alive requires an absolute .exe path.",
                "Canlı tut özelliği tam bir .exe yolu gerektirir.");
        }
        if (!string.IsNullOrWhiteSpace(rule.CpuAffinity) &&
            !TryValidateAffinity(rule.CpuAffinity, out var affinityError))
        {
            return affinityError;
        }
        return null;
    }

    private string? ValidateEditorAndBuildRule(out ProcessRule rule)
    {
        rule = new ProcessRule();
        if (!TryParseInteger(CpuLimitTextBox.Text, allowEmpty: false, out var cpuLimit) ||
            !TryParseInteger(SmartTrimTextBox.Text, allowEmpty: true, out var trim) ||
            !TryParseInteger(ThrottleTriggerTextBox.Text, allowEmpty: true, out var trigger) ||
            !TryParseInteger(ThrottleDurationTextBox.Text, allowEmpty: true, out var duration))
        {
            return Bi(
                "Numeric fields contain an invalid value.",
                "Sayısal alanlardan biri geçersiz bir değer içeriyor.");
        }

        rule = new ProcessRule
        {
            ProcessName = RuleProcessNameTextBox.Text.Trim(),
            CpuPriority = SelectedTag(CpuPriorityComboBox),
            IoPriority = SelectedTag(IoPriorityComboBox),
            CpuAffinity = NullIfEmpty(CpuAffinityTextBox.Text),
            BackgroundOnly = BackgroundOnlyCheckBox.IsChecked == true,
            EcoQoS = EcoQosCheckBox.IsChecked == true,
            Launcher = LauncherCheckBox.IsChecked == true,
            CpuLimit = cpuLimit ?? 0,
            SmartTrimThresholdMb = trim,
            CpuThrottleTriggerPct = trigger,
            CpuThrottleDurationSecs = duration,
            InstanceBalance = InstanceBalanceCheckBox.IsChecked == true,
            Disallowed = DisallowedCheckBox.IsChecked == true,
            KeepAlive = KeepAliveCheckBox.IsChecked == true,
            ExecutablePath = ExecutablePathTextBox.Text.Trim()
        };

        var error = ValidateRule(rule);
        if (error is not null)
        {
            return error;
        }
        var processName = rule.ProcessName;
        var duplicate = _rules
            .Select((candidate, index) => (candidate, index))
            .Any(pair =>
                pair.index != _editingRuleIndex &&
                pair.candidate.ProcessName.Equals(
                    processName,
                    StringComparison.OrdinalIgnoreCase));
        return duplicate
            ? Bi(
                "A rule for this process already exists.",
                "Bu süreç için zaten bir kural var.")
            : null;
    }

    private static bool TryParseInteger(string text, bool allowEmpty, out int? value)
    {
        text = text.Trim();
        if (allowEmpty && text.Length == 0)
        {
            value = null;
            return true;
        }
        if (int.TryParse(text, NumberStyles.Integer, CultureInfo.InvariantCulture, out var parsed))
        {
            value = parsed;
            return true;
        }
        value = null;
        return false;
    }

    private string Bi(string english, string turkish) =>
        LocalizationService.CurrentLanguage == "tr" ? turkish : english;

    private static string? NullIfEmpty(string value) =>
        string.IsNullOrWhiteSpace(value) ? null : value.Trim();

    private string? SelectedTag(System.Windows.Controls.ComboBox comboBox)
    {
        return comboBox.SelectedItem is ComboBoxItem item
            ? NullIfEmpty(item.Tag?.ToString() ?? string.Empty)
            : null;
    }

    private static void SelectTag(System.Windows.Controls.ComboBox comboBox, string? value)
    {
        foreach (var candidate in comboBox.Items.OfType<ComboBoxItem>())
        {
            if (string.Equals(
                    candidate.Tag?.ToString() ?? string.Empty,
                    value ?? string.Empty,
                    StringComparison.OrdinalIgnoreCase))
            {
                comboBox.SelectedItem = candidate;
                return;
            }
        }
        comboBox.SelectedIndex = 0;
    }

    private string ResolveEnginePath()
    {
        var candidates = new[]
        {
            Path.Combine(AppContext.BaseDirectory, "win-ananicy.exe"),
            Path.Combine(AppContext.BaseDirectory, "engine", "win-ananicy.exe"),
            Path.GetFullPath(Path.Combine(AppContext.BaseDirectory, "..", "build", "win-ananicy.exe")),
            Path.GetFullPath(Path.Combine(AppContext.BaseDirectory, "..", "..", "..", "..", "build", "win-ananicy.exe"))
        };
        return candidates.FirstOrDefault(File.Exists) ?? candidates[0];
    }

    private string BuildEngineArguments(bool background)
    {
        var mode = background ? "--background" : "--run";
        return $"{mode} --config \"{_rulesPath}\" --settings \"{_settingsPath}\"";
    }

    private async Task StartEngineAsync(bool showErrors)
    {
        if (IsEngineHealthy(_lastStatus))
        {
            return;
        }
        var enginePath = ResolveEnginePath();
        if (!File.Exists(enginePath))
        {
            if (showErrors)
            {
                ShowError(Bi(
                    "The WinAnanicy engine executable is missing. Repair or reinstall the application.",
                    "WinAnanicy motor dosyası eksik. Uygulamayı onarın veya yeniden kurun."));
            }
            return;
        }

        FooterStatusText.Text = LocalizationService.Text("Starting");
        Process.Start(new ProcessStartInfo
        {
            FileName = enginePath,
            Arguments = BuildEngineArguments(background: true),
            UseShellExecute = false,
            CreateNoWindow = true,
            WorkingDirectory = Path.GetDirectoryName(enginePath)!
        })?.Dispose();

        for (var attempt = 0; attempt < 24; attempt++)
        {
            await Task.Delay(250);
            await RefreshEngineStatusAsync();
            if (IsEngineHealthy(_lastStatus))
            {
                FooterStatusText.Text = LocalizationService.Text("StatusReady");
                return;
            }
        }

        FooterStatusText.Text = LocalizationService.Text("Unhealthy");
        if (showErrors)
        {
            ShowError(Bi(
                "The engine did not report healthy status. Check the Activity page.",
                "Motor sağlıklı durum bildirmedi. Etkinlik sayfasını kontrol edin."));
        }
    }

    private async Task StopEngineAsync()
    {
        if (!IsEngineHealthy(_lastStatus))
        {
            await RefreshEngineStatusAsync();
            return;
        }

        try
        {
            using var stopEvent = EventWaitHandle.OpenExisting(EngineStopEventName);
            stopEvent.Set();
        }
        catch (WaitHandleCannotBeOpenedException)
        {
            return;
        }

        for (var attempt = 0; attempt < 28; attempt++)
        {
            await Task.Delay(250);
            await RefreshEngineStatusAsync();
            if (!IsEngineHealthy(_lastStatus))
            {
                return;
            }
        }
        ShowError(Bi(
            "The engine is taking longer than expected to stop.",
            "Motorun durması beklenenden uzun sürüyor."));
    }

    private void ApplyStartupPreference()
    {
        using var key = Registry.CurrentUser.CreateSubKey(StartupRegistryPath);
        if (_preferences.StartWithWindows)
        {
            var enginePath = ResolveEnginePath();
            if (File.Exists(enginePath))
            {
                key.SetValue(
                    StartupRegistryValue,
                    $"\"{enginePath}\" {BuildEngineArguments(background: true)}",
                    RegistryValueKind.String);
            }
        }
        else
        {
            key.DeleteValue(StartupRegistryValue, throwOnMissingValue: false);
        }
    }

    private static bool IsStartupRegistered()
    {
        using var key = Registry.CurrentUser.OpenSubKey(StartupRegistryPath);
        return key?.GetValue(StartupRegistryValue) is string value &&
               !string.IsNullOrWhiteSpace(value);
    }

    private void ConfigureNotificationIcon()
    {
        _notifyIcon = new WinForms.NotifyIcon
        {
            Text = "WinAnanicy",
            Visible = true,
            Icon = System.Drawing.Icon.ExtractAssociatedIcon(Environment.ProcessPath!)
        };
        _notifyIcon.DoubleClick += (_, _) => ShowFromTray();
        RebuildTrayMenu();
    }

    private void RebuildTrayMenu()
    {
        if (_notifyIcon is null)
        {
            return;
        }
        var menu = new WinForms.ContextMenuStrip();
        var showItem = new WinForms.ToolStripMenuItem(LocalizationService.Text("Show"));
        showItem.Click += (_, _) => ShowFromTray();
        var startItem = new WinForms.ToolStripMenuItem(LocalizationService.Text("Start"));
        startItem.Click += async (_, _) => await StartEngineAsync(showErrors: true);
        var stopItem = new WinForms.ToolStripMenuItem(LocalizationService.Text("Stop"));
        stopItem.Click += async (_, _) => await StopEngineAsync();
        var exitItem = new WinForms.ToolStripMenuItem(LocalizationService.Text("Exit"));
        exitItem.Click += (_, _) =>
        {
            _exitRequested = true;
            Dispatcher.Invoke(Close);
        };
        menu.Items.Add(showItem);
        menu.Items.Add(new WinForms.ToolStripSeparator());
        menu.Items.Add(startItem);
        menu.Items.Add(stopItem);
        menu.Items.Add(new WinForms.ToolStripSeparator());
        menu.Items.Add(exitItem);
        _notifyIcon.ContextMenuStrip?.Dispose();
        _notifyIcon.ContextMenuStrip = menu;
    }

    private void ShowFromTray()
    {
        Dispatcher.Invoke(() =>
        {
            Show();
            WindowState = WindowState.Normal;
            Activate();
        });
    }

    private void HideToTray()
    {
        Hide();
    }

    private void ApplyLanguage(string language)
    {
        _changingLanguage = true;
        LocalizationService.Apply(language);
        SelectTag(LanguageComboBox, language);
        SelectTag(SettingsLanguageComboBox, language);
        UpdateVersionText();
        _preferences.Language = language;
        _changingLanguage = false;
        RebuildTrayMenu();
        _ = SavePreferencesAsync();
        if (_initialized)
        {
            RulesDataGrid.Items.Refresh();
            _ = RefreshEngineStatusAsync();
            _ = RefreshProcessesAsync();
        }
    }

    private void UpdateVersionText()
    {
        var assemblyVersion = Assembly.GetExecutingAssembly().GetName().Version;
        var version = assemblyVersion is null
            ? "unknown"
            : $"{assemblyVersion.Major}.{assemblyVersion.Minor}.{assemblyVersion.Build}";
        VersionText.Text = string.Format(
            CultureInfo.CurrentCulture,
            LocalizationService.Text("Version"),
            version);
    }

    private async Task SavePreferencesAsync()
    {
        try
        {
            await SaveJsonAtomicAsync(_preferencesPath, _preferences, createBackup: false);
        }
        catch
        {
            // Preferences are non-critical; the main rule and engine files stay untouched.
        }
    }

    private async Task RefreshLogAsync()
    {
        try
        {
            if (!File.Exists(_logPath))
            {
                LogTextBox.Text = string.Empty;
                return;
            }
            await using var stream = new FileStream(
                _logPath,
                FileMode.Open,
                FileAccess.Read,
                FileShare.ReadWrite | FileShare.Delete);
            var readLength = (int)Math.Min(stream.Length, 250_000);
            stream.Seek(-readLength, SeekOrigin.End);
            using var reader = new StreamReader(stream);
            LogTextBox.Text = await reader.ReadToEndAsync();
            LogTextBox.ScrollToEnd();
        }
        catch (IOException)
        {
            // The logger can rotate between checking and reading.
        }
    }

    private void OpenRuleEditor(ProcessRule? rule, int index)
    {
        _editingRuleIndex = index;
        rule ??= new ProcessRule();
        RuleProcessNameTextBox.Text = rule.ProcessName;
        SelectTag(PresetComboBox, DetectPreset(rule));
        SelectTag(CpuPriorityComboBox, rule.CpuPriority);
        SelectTag(IoPriorityComboBox, rule.IoPriority);
        CpuAffinityTextBox.Text = rule.CpuAffinity ?? string.Empty;
        CpuLimitTextBox.Text = rule.CpuLimit.ToString(CultureInfo.InvariantCulture);
        SmartTrimTextBox.Text = rule.SmartTrimThresholdMb?.ToString(CultureInfo.InvariantCulture) ?? string.Empty;
        ThrottleTriggerTextBox.Text = rule.CpuThrottleTriggerPct?.ToString(CultureInfo.InvariantCulture) ?? string.Empty;
        ThrottleDurationTextBox.Text = rule.CpuThrottleDurationSecs?.ToString(CultureInfo.InvariantCulture) ?? string.Empty;
        BackgroundOnlyCheckBox.IsChecked = rule.BackgroundOnly;
        EcoQosCheckBox.IsChecked = rule.EcoQoS;
        LauncherCheckBox.IsChecked = rule.Launcher;
        InstanceBalanceCheckBox.IsChecked = rule.InstanceBalance;
        DisallowedCheckBox.IsChecked = rule.Disallowed;
        KeepAliveCheckBox.IsChecked = rule.KeepAlive;
        ExecutablePathTextBox.Text = rule.ExecutablePath;
        ExecutablePathTextBox.IsEnabled = rule.KeepAlive;
        RuleValidationText.Text = string.Empty;
        RuleEditorOverlay.Visibility = Visibility.Visible;
        RuleProcessNameTextBox.Focus();
    }

    private static string? DetectPreset(ProcessRule rule)
    {
        if (rule.CpuPriority == "High" &&
            rule.IoPriority == "High" &&
            !rule.BackgroundOnly &&
            !rule.EcoQoS &&
            rule.CpuLimit == 0)
        {
            return "game";
        }
        if (rule.CpuPriority == "Above Normal" &&
            rule.IoPriority == "Normal" &&
            !rule.BackgroundOnly &&
            rule.CpuLimit == 0)
        {
            return "balanced";
        }
        if (rule.CpuPriority == "Below Normal" &&
            rule.IoPriority == "Low" &&
            rule.BackgroundOnly &&
            rule.EcoQoS &&
            rule.CpuLimit == 50)
        {
            return "background";
        }
        if (rule.CpuPriority == "Idle" &&
            rule.IoPriority == "Very Low" &&
            rule.BackgroundOnly &&
            rule.EcoQoS &&
            rule.CpuLimit == 25)
        {
            return "saver";
        }
        return null;
    }

    private static bool TryValidateAffinity(string text, out string error)
    {
        error = string.Empty;
        text = text.Trim();
        if (text.StartsWith("0x", StringComparison.OrdinalIgnoreCase))
        {
            if (ulong.TryParse(text[2..], NumberStyles.HexNumber, CultureInfo.InvariantCulture, out var mask) &&
                mask != 0)
            {
                return true;
            }
            error = LocalizationService.CurrentLanguage == "tr"
                ? "Geçersiz onaltılık çekirdek maskesi."
                : "Invalid hexadecimal affinity mask.";
            return false;
        }
        if (text.StartsWith("mask:", StringComparison.OrdinalIgnoreCase))
        {
            if (ulong.TryParse(text[5..], NumberStyles.Integer, CultureInfo.InvariantCulture, out var mask) &&
                mask != 0)
            {
                return true;
            }
            error = LocalizationService.CurrentLanguage == "tr"
                ? "Geçersiz ondalık çekirdek maskesi."
                : "Invalid decimal affinity mask.";
            return false;
        }

        var seen = new HashSet<int>();
        foreach (var token in text.Split(',', StringSplitOptions.TrimEntries))
        {
            if (!int.TryParse(token, NumberStyles.Integer, CultureInfo.InvariantCulture, out var core) ||
                core < 0 ||
                core >= IntPtr.Size * 8 ||
                !seen.Add(core))
            {
                error = LocalizationService.CurrentLanguage == "tr"
                    ? "Çekirdek listesi geçersiz, yinelenen veya desteklenmeyen bir dizin içeriyor."
                    : "The core list contains an invalid, duplicate, or unsupported index.";
                return false;
            }
        }
        return seen.Count > 0;
    }

    private void Navigate(string page)
    {
        DashboardPage.Visibility = page == "Dashboard" ? Visibility.Visible : Visibility.Collapsed;
        ProcessesPage.Visibility = page == "Processes" ? Visibility.Visible : Visibility.Collapsed;
        RulesPage.Visibility = page == "Rules" ? Visibility.Visible : Visibility.Collapsed;
        ActivityPage.Visibility = page == "Activity" ? Visibility.Visible : Visibility.Collapsed;
        SettingsPage.Visibility = page == "Settings" ? Visibility.Visible : Visibility.Collapsed;

        DashboardNavButton.Tag = page == "Dashboard" ? "Selected" : null;
        ProcessesNavButton.Tag = page == "Processes" ? "Selected" : null;
        RulesNavButton.Tag = page == "Rules" ? "Selected" : null;
        ActivityNavButton.Tag = page == "Activity" ? "Selected" : null;
        SettingsNavButton.Tag = page == "Settings" ? "Selected" : null;

        if (page == "Processes") _ = RefreshProcessesAsync();
        if (page == "Activity") _ = RefreshLogAsync();
    }

    private void ShowError(string message)
    {
        MessageBox.Show(
            this,
            message,
            LocalizationService.Text("Error"),
            MessageBoxButton.OK,
            MessageBoxImage.Error);
    }

    private void NavigationButton_Click(object sender, RoutedEventArgs e)
    {
        if (sender is Button button && button.CommandParameter is string page)
        {
            Navigate(page);
        }
    }

    private void GoProcessesButton_Click(object sender, RoutedEventArgs e) => Navigate("Processes");

    private async void StartEngineButton_Click(object sender, RoutedEventArgs e) =>
        await StartEngineAsync(showErrors: true);

    private async void StopEngineButton_Click(object sender, RoutedEventArgs e) =>
        await StopEngineAsync();

    private async void RestartEngineButton_Click(object sender, RoutedEventArgs e)
    {
        await StopEngineAsync();
        await StartEngineAsync(showErrors: true);
    }

    private void ProcessSearchTextBox_TextChanged(object sender, TextChangedEventArgs e) =>
        ApplyProcessFilter();

    private async void RefreshProcessesButton_Click(object sender, RoutedEventArgs e) =>
        await RefreshProcessesAsync();

    private void AddRuleFromProcessButton_Click(object sender, RoutedEventArgs e)
    {
        if (ProcessesDataGrid.SelectedItem is not ProcessItem process)
        {
            ShowError(LocalizationService.Text("SelectProcess"));
            return;
        }
        var existingIndex = _rules
            .Select((rule, index) => (rule, index))
            .FirstOrDefault(pair => pair.rule.ProcessName.Equals(
                process.Name,
                StringComparison.OrdinalIgnoreCase))
            .index;
        var existing = _rules.FirstOrDefault(rule => rule.ProcessName.Equals(
            process.Name,
            StringComparison.OrdinalIgnoreCase));
        OpenRuleEditor(
            existing?.Clone() ?? new ProcessRule { ProcessName = process.Name },
            existing is null ? -1 : existingIndex);
    }

    private void ProcessesDataGrid_MouseDoubleClick(object sender, MouseButtonEventArgs e) =>
        AddRuleFromProcessButton_Click(sender, e);

    private void NewRuleButton_Click(object sender, RoutedEventArgs e) =>
        OpenRuleEditor(null, -1);

    private void EditRuleButton_Click(object sender, RoutedEventArgs e)
    {
        if (RulesDataGrid.SelectedItem is not ProcessRule rule)
        {
            ShowError(LocalizationService.Text("SelectRule"));
            return;
        }
        OpenRuleEditor(rule.Clone(), _rules.IndexOf(rule));
    }

    private void RulesDataGrid_MouseDoubleClick(object sender, MouseButtonEventArgs e) =>
        EditRuleButton_Click(sender, e);

    private async void DeleteRuleButton_Click(object sender, RoutedEventArgs e)
    {
        if (RulesDataGrid.SelectedItem is not ProcessRule rule)
        {
            ShowError(LocalizationService.Text("SelectRule"));
            return;
        }
        if (MessageBox.Show(
                this,
                LocalizationService.Text("ConfirmDelete"),
                LocalizationService.Text("ConfirmTitle"),
                MessageBoxButton.YesNo,
                MessageBoxImage.Question) != MessageBoxResult.Yes)
        {
            return;
        }
        _rules.Remove(rule);
        await SaveRulesAsync();
    }

    private async void SaveRuleButton_Click(object sender, RoutedEventArgs e)
    {
        var error = ValidateEditorAndBuildRule(out var rule);
        if (error is not null)
        {
            RuleValidationText.Text = error;
            return;
        }
        if (_editingRuleIndex >= 0)
        {
            _rules[_editingRuleIndex] = rule;
        }
        else
        {
            _rules.Add(rule);
        }

        var sorted = _rules.OrderBy(item => item.ProcessName, StringComparer.OrdinalIgnoreCase).ToList();
        _rules.Clear();
        foreach (var item in sorted) _rules.Add(item);
        await SaveRulesAsync();
        RuleEditorOverlay.Visibility = Visibility.Collapsed;
    }

    private void CancelRuleButton_Click(object sender, RoutedEventArgs e) =>
        RuleEditorOverlay.Visibility = Visibility.Collapsed;

    private void KeepAliveCheckBox_Changed(object sender, RoutedEventArgs e)
    {
        if (ExecutablePathTextBox is not null)
        {
            ExecutablePathTextBox.IsEnabled = KeepAliveCheckBox.IsChecked == true;
        }
    }

    private void PresetComboBox_SelectionChanged(object sender, SelectionChangedEventArgs e)
    {
        if (PresetComboBox.SelectedItem is not ComboBoxItem item)
        {
            return;
        }
        switch (item.Tag?.ToString())
        {
            case null:
            case "":
                return;
        }

        ThrottleTriggerTextBox.Text = string.Empty;
        ThrottleDurationTextBox.Text = string.Empty;
        InstanceBalanceCheckBox.IsChecked = false;
        DisallowedCheckBox.IsChecked = false;
        KeepAliveCheckBox.IsChecked = false;
        LauncherCheckBox.IsChecked = false;
        ExecutablePathTextBox.Text = string.Empty;

        switch (item.Tag?.ToString())
        {
            case "game":
                SelectTag(CpuPriorityComboBox, "High");
                SelectTag(IoPriorityComboBox, "High");
                CpuAffinityTextBox.Text = string.Empty;
                CpuLimitTextBox.Text = "0";
                SmartTrimTextBox.Text = string.Empty;
                BackgroundOnlyCheckBox.IsChecked = false;
                EcoQosCheckBox.IsChecked = false;
                break;
            case "balanced":
                SelectTag(CpuPriorityComboBox, "Above Normal");
                SelectTag(IoPriorityComboBox, "Normal");
                CpuAffinityTextBox.Text = string.Empty;
                CpuLimitTextBox.Text = "0";
                SmartTrimTextBox.Text = string.Empty;
                BackgroundOnlyCheckBox.IsChecked = false;
                EcoQosCheckBox.IsChecked = false;
                break;
            case "background":
                SelectTag(CpuPriorityComboBox, "Below Normal");
                SelectTag(IoPriorityComboBox, "Low");
                CpuAffinityTextBox.Text = string.Empty;
                CpuLimitTextBox.Text = "50";
                SmartTrimTextBox.Text = "2048";
                BackgroundOnlyCheckBox.IsChecked = true;
                EcoQosCheckBox.IsChecked = true;
                break;
            case "saver":
                SelectTag(CpuPriorityComboBox, "Idle");
                SelectTag(IoPriorityComboBox, "Very Low");
                CpuAffinityTextBox.Text = string.Empty;
                CpuLimitTextBox.Text = "25";
                SmartTrimTextBox.Text = "1024";
                BackgroundOnlyCheckBox.IsChecked = true;
                EcoQosCheckBox.IsChecked = true;
                break;
        }
    }

    private void BrowseExecutableButton_Click(object sender, RoutedEventArgs e)
    {
        var dialog = new OpenFileDialog
        {
            Filter = $"{LocalizationService.Text("ExecutableFiles")} (*.exe)|*.exe",
            CheckFileExists = true
        };
        if (dialog.ShowDialog(this) == true)
        {
            ExecutablePathTextBox.Text = dialog.FileName;
            KeepAliveCheckBox.IsChecked = true;
        }
    }

    private async void ImportRulesButton_Click(object sender, RoutedEventArgs e)
    {
        var dialog = new OpenFileDialog
        {
            Filter = $"{LocalizationService.Text("JsonFiles")} (*.json)|*.json",
            CheckFileExists = true
        };
        if (dialog.ShowDialog(this) != true) return;
        try
        {
            var imported = JsonSerializer.Deserialize<List<ProcessRule>>(
                               await File.ReadAllTextAsync(dialog.FileName),
                               _jsonOptions) ??
                           [];
            var validation = ValidateRuleSet(imported);
            if (validation is not null) throw new InvalidDataException(validation);

            foreach (var rule in imported)
            {
                var existing = _rules.FirstOrDefault(candidate =>
                    candidate.ProcessName.Equals(rule.ProcessName, StringComparison.OrdinalIgnoreCase));
                if (existing is not null) _rules[_rules.IndexOf(existing)] = rule;
                else _rules.Add(rule);
            }
            await SaveRulesAsync();
            MessageBox.Show(
                this,
                LocalizationService.Text("ImportComplete"),
                "WinAnanicy",
                MessageBoxButton.OK,
                MessageBoxImage.Information);
        }
        catch (Exception exception)
        {
            ShowError(exception.Message);
        }
    }

    private async void ExportRulesButton_Click(object sender, RoutedEventArgs e)
    {
        var dialog = new SaveFileDialog
        {
            Filter = $"{LocalizationService.Text("JsonFiles")} (*.json)|*.json",
            FileName = $"win-ananicy-rules-{DateTime.Now:yyyyMMdd}.json",
            AddExtension = true,
            DefaultExt = ".json"
        };
        if (dialog.ShowDialog(this) != true) return;
        await File.WriteAllTextAsync(
            dialog.FileName,
            JsonSerializer.Serialize(_rules.ToList(), _jsonOptions));
        MessageBox.Show(
            this,
            LocalizationService.Text("ExportComplete"),
            "WinAnanicy",
            MessageBoxButton.OK,
            MessageBoxImage.Information);
    }

    private async void RefreshLogButton_Click(object sender, RoutedEventArgs e) =>
        await RefreshLogAsync();

    private void OpenLogFolderButton_Click(object sender, RoutedEventArgs e) =>
        OpenFolder(Path.GetDirectoryName(_logPath)!);

    private void OpenDataFolderButton_Click(object sender, RoutedEventArgs e) =>
        OpenFolder(_dataDirectory);

    private static void OpenFolder(string path)
    {
        Directory.CreateDirectory(path);
        Process.Start(new ProcessStartInfo
        {
            FileName = path,
            UseShellExecute = true
        })?.Dispose();
    }

    private async void SaveSettingsButton_Click(object sender, RoutedEventArgs e)
    {
        if (!int.TryParse(PollIntervalTextBox.Text, out var poll) ||
            poll is < 250 or > 5000 ||
            !int.TryParse(TrimCooldownTextBox.Text, out var cooldown) ||
            cooldown is < 5 or > 3600 ||
            !int.TryParse(WatchdogRetriesTextBox.Text, out var retries) ||
            retries is < 0 or > 20)
        {
            ShowError(Bi(
                "Use 250–5000 ms, 5–3600 seconds, and 0–20 retries.",
                "250–5000 ms, 5–3600 saniye ve 0–20 yeniden deneme kullanın."));
            return;
        }

        _engineSettings = new EngineSettings
        {
            PowerPlanEnabled = PowerPlanCheckBox.IsChecked == true,
            PollIntervalMs = poll,
            SmartTrimCooldownSecs = cooldown,
            WatchdogMaxRetries = retries
        };
        _preferences.StartWithWindows = StartupCheckBox.IsChecked == true;
        _preferences.MinimizeToTray = MinimizeTrayCheckBox.IsChecked == true;
        await SaveJsonAtomicAsync(_settingsPath, _engineSettings, createBackup: true);
        await SavePreferencesAsync();
        ApplyStartupPreference();
        FooterStatusText.Text = LocalizationService.Text("Saved");
    }

    private void LanguageComboBox_SelectionChanged(object sender, SelectionChangedEventArgs e)
    {
        if (_changingLanguage || LanguageComboBox.SelectedItem is not ComboBoxItem item) return;
        ApplyLanguage(item.Tag?.ToString() ?? "en");
    }

    private void SettingsLanguageComboBox_SelectionChanged(object sender, SelectionChangedEventArgs e)
    {
        if (_changingLanguage || SettingsLanguageComboBox.SelectedItem is not ComboBoxItem item) return;
        ApplyLanguage(item.Tag?.ToString() ?? "en");
    }

    private void MinimizeButton_Click(object sender, RoutedEventArgs e) =>
        WindowState = WindowState.Minimized;

    private void MaximizeButton_Click(object sender, RoutedEventArgs e) =>
        WindowState = WindowState == WindowState.Maximized
            ? WindowState.Normal
            : WindowState.Maximized;

    private void CloseButton_Click(object sender, RoutedEventArgs e) => Close();

    private void Window_Closing(object? sender, System.ComponentModel.CancelEventArgs e)
    {
        if (!_exitRequested && _preferences.MinimizeToTray)
        {
            e.Cancel = true;
            HideToTray();
            return;
        }
        _statusTimer.Stop();
        _processTimer.Stop();
        _notifyIcon?.Dispose();
    }
}
