using System.Text.Json.Serialization;

namespace WinAnanicyGui;

public sealed class ProcessRule
{
    [JsonPropertyName("process_name")]
    public string ProcessName { get; set; } = string.Empty;

    [JsonPropertyName("cpu_priority")]
    public string? CpuPriority { get; set; }

    [JsonPropertyName("io_priority")]
    public string? IoPriority { get; set; }

    [JsonPropertyName("cpu_affinity")]
    public string? CpuAffinity { get; set; }

    [JsonPropertyName("background_only")]
    public bool BackgroundOnly { get; set; }

    [JsonPropertyName("eco_qos")]
    public bool EcoQoS { get; set; }

    [JsonPropertyName("launcher")]
    public bool Launcher { get; set; }

    [JsonPropertyName("cpu_limit")]
    public int CpuLimit { get; set; }

    [JsonPropertyName("smart_trim_threshold_mb")]
    public int? SmartTrimThresholdMb { get; set; }

    [JsonPropertyName("cpu_throttle_trigger_pct")]
    public int? CpuThrottleTriggerPct { get; set; }

    [JsonPropertyName("cpu_throttle_duration_secs")]
    public int? CpuThrottleDurationSecs { get; set; }

    [JsonPropertyName("instance_balance")]
    public bool InstanceBalance { get; set; }

    [JsonPropertyName("disallowed")]
    public bool Disallowed { get; set; }

    [JsonPropertyName("keep_alive")]
    public bool KeepAlive { get; set; }

    [JsonPropertyName("executable_path")]
    public string ExecutablePath { get; set; } = string.Empty;

    [JsonIgnore]
    public string ProfileSummary
    {
        get
        {
            var parts = new List<string>();
            if (!string.IsNullOrWhiteSpace(CpuPriority)) parts.Add($"CPU: {CpuPriority}");
            if (CpuLimit > 0) parts.Add($"{CpuLimit}%");
            if (EcoQoS) parts.Add("EcoQoS");
            if (BackgroundOnly) parts.Add(LocalizationService.Text("BackgroundShort"));
            if (parts.Count == 0) parts.Add(LocalizationService.Text("MonitorShort"));
            return string.Join(" · ", parts);
        }
    }

    public ProcessRule Clone() => (ProcessRule)MemberwiseClone();
}
