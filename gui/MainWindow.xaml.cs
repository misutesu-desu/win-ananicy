using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.IO;
using System.Linq;
using System.Security.Principal;
using System.ServiceProcess;
using System.Text.Json;
using System.Threading.Tasks;
using System.Windows;
using System.Windows.Controls;
using System.Windows.Input;
using System.Windows.Threading;

namespace WinAnanicyGui
{
    /// <summary>
    /// Represents a process item shown in the processes list
    /// </summary>
    public class ProcessItem
    {
        public string Name { get; set; } = string.Empty;
        public int Id { get; set; }
        public string Title { get; set; } = string.Empty;
    }

    public partial class MainWindow : Window
    {
        private const string ServiceName = "WinAnanicy";
        
        private string _rulesPath = string.Empty;
        private List<ProcessRule> _rules = new();
        private List<ProcessItem> _processes = new();
        
        private ServiceController? _serviceController;
        private DispatcherTimer _refreshTimer = new();
        private bool _isAdmin = false;
        private ProcessRule? _currentEditingRule = null;

        public MainWindow()
        {
            InitializeComponent();
            
            // Check administrative permissions
            _isAdmin = IsRunningAsAdmin();
            AdminBadge.Visibility = _isAdmin ? Visibility.Visible : Visibility.Collapsed;
            AdminWarningTextBlock.Visibility = _isAdmin ? Visibility.Collapsed : Visibility.Visible;
            ElevateButton.Visibility = _isAdmin ? Visibility.Collapsed : Visibility.Visible;

            // Setup service controller
            try
            {
                _serviceController = new ServiceController(ServiceName);
            }
            catch
            {
                _serviceController = null;
            }

            // Setup timer to refresh processes and service status
            _refreshTimer.Interval = TimeSpan.FromSeconds(2);
            _refreshTimer.Tick += RefreshTimer_Tick;
        }

        private void Window_Loaded(object sender, RoutedEventArgs e)
        {
            InitRulesPath();
            InitAffinityPanel();
            LoadRules();
            RefreshProcessesAsync();
            RefreshServiceStatus();
            
            _refreshTimer.Start();
        }

        private void RefreshTimer_Tick(object? sender, EventArgs e)
        {
            RefreshServiceStatus();
            // We periodically refresh processes, but not aggressively to avoid UI flickering
            if (ModalOverlay.Visibility == Visibility.Collapsed)
            {
                RefreshProcessesAsync();
            }
        }

        #region Initialization

        private void InitRulesPath()
        {
            string localPath = Path.Combine(AppDomain.CurrentDomain.BaseDirectory, "rules.json");
            
            if (File.Exists(localPath))
            {
                _rulesPath = localPath;
            }
            else
            {
                // Try walking up in case of execution inside visual studio build directories
                string walkPath = AppDomain.CurrentDomain.BaseDirectory;
                bool found = false;
                for (int i = 0; i < 5; i++)
                {
                    walkPath = Path.GetDirectoryName(walkPath)!;
                    if (string.IsNullOrEmpty(walkPath)) break;
                    
                    string checkPath = Path.Combine(walkPath, "rules.json");
                    if (File.Exists(checkPath))
                    {
                        _rulesPath = checkPath;
                        found = true;
                        break;
                    }
                }
                
                if (!found)
                {
                    _rulesPath = localPath;
                }
            }

            RulesPathTextBlock.Text = $"Config: {_rulesPath}";
        }

        private void InitAffinityPanel()
        {
            AffinityCoresPanel.Children.Clear();
            int coreCount = Environment.ProcessorCount;
            for (int i = 0; i < coreCount; i++)
            {
                var cb = new CheckBox
                {
                    Content = $"Core {i}",
                    Tag = i,
                    Margin = new Thickness(0, 0, 16, 8)
                };
                AffinityCoresPanel.Children.Add(cb);
            }
        }

        #endregion

        #region Rules Management

        private void LoadRules()
        {
            try
            {
                if (File.Exists(_rulesPath))
                {
                    string json = File.ReadAllText(_rulesPath);
                    _rules = JsonSerializer.Deserialize<List<ProcessRule>>(json) ?? new List<ProcessRule>();
                }
                else
                {
                    _rules = new List<ProcessRule>();
                }
            }
            catch (Exception ex)
            {
                MessageBox.Show($"Failed to load rules: {ex.Message}", "Error", MessageBoxButton.OK, MessageBoxImage.Error);
                _rules = new List<ProcessRule>();
            }

            RulesDataGrid.ItemsSource = null;
            RulesDataGrid.ItemsSource = _rules;
        }

        private void SaveRules()
        {
            try
            {
                var options = new JsonSerializerOptions { WriteIndented = true };
                string json = JsonSerializer.Serialize(_rules, options);
                File.WriteAllText(_rulesPath, json);
                LoadRules(); // Reload grid
            }
            catch (Exception ex)
            {
                MessageBox.Show($"Failed to save rules: {ex.Message}", "Error", MessageBoxButton.OK, MessageBoxImage.Error);
            }
        }

        #endregion

        #region Process Monitoring

        private async void RefreshProcessesAsync()
        {
            string filterText = SearchTextBox.Text.Trim();
            
            var list = await Task.Run(() =>
            {
                var items = new List<ProcessItem>();
                var processes = Process.GetProcesses();
                
                foreach (var p in processes)
                {
                    try
                    {
                        if (p.Id == 0 || string.IsNullOrEmpty(p.ProcessName)) continue;
                        
                        string procName = p.ProcessName.ToLower();
                        if (!procName.EndsWith(".exe")) procName += ".exe";

                        items.Add(new ProcessItem
                        {
                            Id = p.Id,
                            Name = procName,
                            Title = p.MainWindowTitle
                        });
                    }
                    catch
                    {
                        // Ignore access denied system processes
                    }
                }

                // Sort processes: windows with titles first, then alphabetically by name
                return items
                    .OrderByDescending(x => !string.IsNullOrEmpty(x.Title))
                    .ThenBy(x => x.Name)
                    .ToList();
            });

            _processes = list;
            ApplyProcessFilter(filterText);
        }

        private void ApplyProcessFilter(string filter)
        {
            if (string.IsNullOrEmpty(filter))
            {
                ProcessesDataGrid.ItemsSource = _processes;
            }
            else
            {
                ProcessesDataGrid.ItemsSource = _processes
                    .Where(p => p.Name.Contains(filter, StringComparison.OrdinalIgnoreCase) || 
                                p.Title.Contains(filter, StringComparison.OrdinalIgnoreCase) || 
                                p.Id.ToString().Contains(filter))
                    .ToList();
            }
        }

        private void SearchTextBox_TextChanged(object sender, TextChangedEventArgs e)
        {
            ApplyProcessFilter(SearchTextBox.Text.Trim());
        }

        private void RefreshButton_Click(object sender, RoutedEventArgs e)
        {
            RefreshProcessesAsync();
        }

        #endregion

        #region Rule Builder Overlay

        private void OpenRuleModal(string processName, ProcessRule? existingRule = null)
        {
            _currentEditingRule = existingRule;
            ModalProcessNameTextBox.Text = processName;
            ModalProcessNameTextBox.IsReadOnly = existingRule != null; // Lock name if editing existing rule

            if (existingRule != null)
            {
                ModalTitleTextBlock.Text = $"Edit Rule: {processName}";
                
                // Select CPU Priority
                SelectComboBoxItem(CpuPriorityComboBox, existingRule.CpuPriority ?? "Normal");
                
                // Select I/O Priority
                SelectComboBoxItem(IoPriorityComboBox, existingRule.IoPriority ?? "Normal");
                
                // Check Cores
                SetCoreCheckboxes(existingRule.CpuAffinity);
                
                // Background Only
                BackgroundOnlyCheckBox.IsChecked = existingRule.BackgroundOnly ?? false;
            }
            else
            {
                ModalTitleTextBlock.Text = $"Configure Rule: {processName}";
                
                // Default options
                CpuPriorityComboBox.SelectedIndex = 2; // Normal
                IoPriorityComboBox.SelectedIndex = 2;  // Normal
                SetCoreCheckboxes(null);               // Check all by default
                BackgroundOnlyCheckBox.IsChecked = false;
            }

            ModalOverlay.Visibility = Visibility.Visible;
        }

        private void SelectComboBoxItem(ComboBox combo, string value)
        {
            for (int i = 0; i < combo.Items.Count; i++)
            {
                if (combo.Items[i] is ComboBoxItem item && item.Content.ToString()!.Equals(value, StringComparison.OrdinalIgnoreCase))
                {
                    combo.SelectedIndex = i;
                    return;
                }
            }
            combo.SelectedIndex = 2; // Fallback
        }

        private void SetCoreCheckboxes(string? affinityStr)
        {
            if (string.IsNullOrEmpty(affinityStr))
            {
                // Select all cores
                foreach (var child in AffinityCoresPanel.Children)
                {
                    if (child is CheckBox cb) cb.IsChecked = true;
                }
                return;
            }

            var activeCores = new List<int>();
            
            // 1. Hex parsing
            if (affinityStr.StartsWith("0x", StringComparison.OrdinalIgnoreCase) || 
                affinityStr.StartsWith("0X", StringComparison.OrdinalIgnoreCase))
            {
                if (long.TryParse(affinityStr.Substring(2), System.Globalization.NumberStyles.HexNumber, null, out long mask))
                {
                    for (int i = 0; i < 64; i++)
                    {
                        if ((mask & (1L << i)) != 0) activeCores.Add(i);
                    }
                }
            }
            // 2. Comma separated
            else if (affinityStr.Contains(','))
            {
                var tokens = affinityStr.Split(',');
                foreach (var token in tokens)
                {
                    if (int.TryParse(token.Trim(), out int core)) activeCores.Add(core);
                }
            }
            // 3. Single decimal value
            else
            {
                if (long.TryParse(affinityStr, out long val))
                {
                    if (affinityStr.Length == 1 && val < 10)
                    {
                        activeCores.Add((int)val);
                    }
                    else
                    {
                        for (int i = 0; i < 64; i++)
                        {
                            if ((val & (1L << i)) != 0) activeCores.Add(i);
                        }
                    }
                }
            }

            foreach (var child in AffinityCoresPanel.Children)
            {
                if (child is CheckBox cb && cb.Tag is int coreIdx)
                {
                    cb.IsChecked = activeCores.Contains(coreIdx);
                }
            }
        }

        private string GetSelectedCoresString()
        {
            var selected = new List<int>();
            int coreCount = Environment.ProcessorCount;
            
            foreach (var child in AffinityCoresPanel.Children)
            {
                if (child is CheckBox cb && cb.IsChecked == true && cb.Tag is int coreIdx)
                {
                    selected.Add(coreIdx);
                }
            }

            if (selected.Count == coreCount)
            {
                // If all cores are selected, we can omit affinity or set empty string (defaults to all)
                return "";
            }

            return string.Join(",", selected);
        }

        private void SaveModalButton_Click(object sender, RoutedEventArgs e)
        {
            string procName = ModalProcessNameTextBox.Text.Trim();
            if (string.IsNullOrEmpty(procName))
            {
                MessageBox.Show("Please enter a valid process file name.", "Validation Error", MessageBoxButton.OK, MessageBoxImage.Warning);
                return;
            }

            if (!procName.EndsWith(".exe", StringComparison.OrdinalIgnoreCase))
            {
                procName += ".exe";
            }

            string cpuPriority = (CpuPriorityComboBox.SelectedItem as ComboBoxItem)?.Content.ToString() ?? "Normal";
            string ioPriority = (IoPriorityComboBox.SelectedItem as ComboBoxItem)?.Content.ToString() ?? "Normal";
            string cpuAffinity = GetSelectedCoresString();
            bool backgroundOnly = BackgroundOnlyCheckBox.IsChecked ?? false;

            var rule = _currentEditingRule ?? new ProcessRule();
            rule.ProcessName = procName;
            rule.CpuPriority = cpuPriority;
            rule.IoPriority = ioPriority;
            rule.CpuAffinity = string.IsNullOrEmpty(cpuAffinity) ? null : cpuAffinity;
            rule.BackgroundOnly = backgroundOnly ? true : (bool?)null;

            if (_currentEditingRule == null)
            {
                // Prevent duplicate rules
                var existing = _rules.FirstOrDefault(r => r.ProcessName.Equals(procName, StringComparison.OrdinalIgnoreCase));
                if (existing != null)
                {
                    existing.CpuPriority = cpuPriority;
                    existing.IoPriority = ioPriority;
                    existing.CpuAffinity = string.IsNullOrEmpty(cpuAffinity) ? null : cpuAffinity;
                    existing.BackgroundOnly = backgroundOnly ? true : (bool?)null;
                }
                else
                {
                    _rules.Add(rule);
                }
            }

            SaveRules();
            ModalOverlay.Visibility = Visibility.Collapsed;
        }

        private void CancelModalButton_Click(object sender, RoutedEventArgs e)
        {
            ModalOverlay.Visibility = Visibility.Collapsed;
        }

        private void SelectAllCores_Click(object sender, RoutedEventArgs e)
        {
            foreach (var child in AffinityCoresPanel.Children)
            {
                if (child is CheckBox cb) cb.IsChecked = true;
            }
        }

        private void ClearAllCores_Click(object sender, RoutedEventArgs e)
        {
            foreach (var child in AffinityCoresPanel.Children)
            {
                if (child is CheckBox cb) cb.IsChecked = false;
            }
        }

        #endregion

        #region Grid Action Handlers

        private void OptimizeButton_Click(object sender, RoutedEventArgs e)
        {
            if (sender is Button btn && btn.DataContext is ProcessItem item)
            {
                var existingRule = _rules.FirstOrDefault(r => r.ProcessName.Equals(item.Name, StringComparison.OrdinalIgnoreCase));
                OpenRuleModal(item.Name, existingRule);
            }
        }

        private void ProcessesDataGrid_MouseDoubleClick(object sender, MouseButtonEventArgs e)
        {
            if (ProcessesDataGrid.SelectedItem is ProcessItem item)
            {
                var existingRule = _rules.FirstOrDefault(r => r.ProcessName.Equals(item.Name, StringComparison.OrdinalIgnoreCase));
                OpenRuleModal(item.Name, existingRule);
            }
        }

        private void AddManualRuleButton_Click(object sender, RoutedEventArgs e)
        {
            OpenRuleModal("");
        }

        private void EditRuleButton_Click(object sender, RoutedEventArgs e)
        {
            if (sender is Button btn && btn.DataContext is ProcessRule rule)
            {
                OpenRuleModal(rule.ProcessName, rule);
            }
        }

        private void DeleteRuleButton_Click(object sender, RoutedEventArgs e)
        {
            if (sender is Button btn && btn.DataContext is ProcessRule rule)
            {
                var result = MessageBox.Show($"Are you sure you want to delete the rule for {rule.ProcessName}?", "Confirm Delete", MessageBoxButton.YesNo, MessageBoxImage.Question);
                if (result == MessageBoxResult.Yes)
                {
                    _rules.Remove(rule);
                    SaveRules();
                }
            }
        }

        #endregion

        #region Windows Service Management

        private void RefreshServiceStatus()
        {
            if (_serviceController == null)
            {
                ServiceStatusIndicator.Fill = System.Windows.Media.Brushes.DarkGray;
                ServiceStatusTextBlock.Text = "Service: Not Found";
                StartServiceButton.IsEnabled = false;
                StopServiceButton.IsEnabled = false;
                RestartServiceButton.IsEnabled = false;
                return;
            }

            try
            {
                _serviceController.Refresh();
                var status = _serviceController.Status;
                
                switch (status)
                {
                    case ServiceControllerStatus.Running:
                        ServiceStatusIndicator.Fill = System.Windows.Media.Brushes.LimeGreen;
                        ServiceStatusTextBlock.Text = "Service: Running";
                        StartServiceButton.IsEnabled = false;
                        StopServiceButton.IsEnabled = _isAdmin;
                        RestartServiceButton.IsEnabled = _isAdmin;
                        break;
                    case ServiceControllerStatus.Stopped:
                        ServiceStatusIndicator.Fill = System.Windows.Media.Brushes.Crimson;
                        ServiceStatusTextBlock.Text = "Service: Stopped";
                        StartServiceButton.IsEnabled = _isAdmin;
                        StopServiceButton.IsEnabled = false;
                        RestartServiceButton.IsEnabled = false;
                        break;
                    case ServiceControllerStatus.StartPending:
                    case ServiceControllerStatus.StopPending:
                    case ServiceControllerStatus.ContinuePending:
                    case ServiceControllerStatus.PausePending:
                        ServiceStatusIndicator.Fill = System.Windows.Media.Brushes.Orange;
                        ServiceStatusTextBlock.Text = "Service: Pending...";
                        StartServiceButton.IsEnabled = false;
                        StopServiceButton.IsEnabled = false;
                        RestartServiceButton.IsEnabled = false;
                        break;
                    default:
                        ServiceStatusIndicator.Fill = System.Windows.Media.Brushes.DarkGray;
                        ServiceStatusTextBlock.Text = "Service: Unknown";
                        break;
                }
            }
            catch
            {
                // Likely access denied or service doesn't exist
                ServiceStatusIndicator.Fill = System.Windows.Media.Brushes.Crimson;
                ServiceStatusTextBlock.Text = "Service: Error Querying";
                StartServiceButton.IsEnabled = false;
                StopServiceButton.IsEnabled = false;
                RestartServiceButton.IsEnabled = false;
            }
        }

        private void StartServiceButton_Click(object sender, RoutedEventArgs e)
        {
            if (!_isAdmin)
            {
                MessageBox.Show("Administrator privileges are required to start services. Please click 'Restart as Administrator' first.", "Access Denied", MessageBoxButton.OK, MessageBoxImage.Warning);
                return;
            }

            try
            {
                _serviceController?.Start();
                RefreshServiceStatus();
            }
            catch (Exception ex)
            {
                MessageBox.Show($"Failed to start service: {ex.Message}", "Error", MessageBoxButton.OK, MessageBoxImage.Error);
            }
        }

        private void StopServiceButton_Click(object sender, RoutedEventArgs e)
        {
            if (!_isAdmin)
            {
                MessageBox.Show("Administrator privileges are required to stop services. Please click 'Restart as Administrator' first.", "Access Denied", MessageBoxButton.OK, MessageBoxImage.Warning);
                return;
            }

            try
            {
                _serviceController?.Stop();
                RefreshServiceStatus();
            }
            catch (Exception ex)
            {
                MessageBox.Show($"Failed to stop service: {ex.Message}", "Error", MessageBoxButton.OK, MessageBoxImage.Error);
            }
        }

        private void RestartServiceButton_Click(object sender, RoutedEventArgs e)
        {
            if (!_isAdmin)
            {
                MessageBox.Show("Administrator privileges are required to restart services. Please click 'Restart as Administrator' first.", "Access Denied", MessageBoxButton.OK, MessageBoxImage.Warning);
                return;
            }

            try
            {
                _serviceController?.Stop();
                _serviceController?.WaitForStatus(ServiceControllerStatus.Stopped, TimeSpan.FromSeconds(5));
                _serviceController?.Start();
                RefreshServiceStatus();
            }
            catch (Exception ex)
            {
                MessageBox.Show($"Failed to restart service: {ex.Message}", "Error", MessageBoxButton.OK, MessageBoxImage.Error);
            }
        }

        private void ElevateButton_Click(object sender, RoutedEventArgs e)
        {
            var processInfo = new ProcessStartInfo
            {
                FileName = Environment.ProcessPath,
                UseShellExecute = true,
                Verb = "runas"
            };
            
            try
            {
                Process.Start(processInfo);
                Application.Current.Shutdown();
            }
            catch
            {
                // User cancelled UAC prompt
            }
        }

        #endregion

        #region Helpers

        private bool IsRunningAsAdmin()
        {
            using (WindowsIdentity identity = WindowsIdentity.GetCurrent())
            {
                WindowsPrincipal principal = new WindowsPrincipal(identity);
                return principal.IsInRole(WindowsBuiltInRole.Administrator);
            }
        }

        #endregion
    }
}