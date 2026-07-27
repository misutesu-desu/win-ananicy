using System.Text.Json.Serialization;

namespace WinAnanicyGui;

public sealed class EngineSettings
{
    [JsonPropertyName("power_plan_enabled")]
    public bool PowerPlanEnabled { get; set; } = true;

    [JsonPropertyName("poll_interval_ms")]
    public int PollIntervalMs { get; set; } = 1000;

    [JsonPropertyName("smart_trim_cooldown_secs")]
    public int SmartTrimCooldownSecs { get; set; } = 30;

    [JsonPropertyName("watchdog_max_retries")]
    public int WatchdogMaxRetries { get; set; } = 5;
}

public sealed class UiPreferences
{
    public string Language { get; set; } = "en";
    public bool StartWithWindows { get; set; } = true;
    public bool MinimizeToTray { get; set; } = true;
}

public sealed class EngineStatus
{
    [JsonPropertyName("version")]
    public string Version { get; set; } = "1.0.0";

    [JsonPropertyName("running")]
    public bool Running { get; set; }

    [JsonPropertyName("pid")]
    public int Pid { get; set; }

    [JsonPropertyName("started_at_utc")]
    public DateTimeOffset StartedAtUtc { get; set; }

    [JsonPropertyName("last_cycle_utc")]
    public DateTimeOffset LastCycleUtc { get; set; }

    [JsonPropertyName("rules_loaded")]
    public int RulesLoaded { get; set; }

    [JsonPropertyName("matched_processes")]
    public int MatchedProcesses { get; set; }

    [JsonPropertyName("applied_processes")]
    public int AppliedProcesses { get; set; }

    [JsonPropertyName("power_plan_active")]
    public bool PowerPlanActive { get; set; }
}

public sealed class ProcessItem
{
    public string Name { get; init; } = string.Empty;
    public int Id { get; init; }
    public double CpuPercent { get; init; }
    public long MemoryMb { get; init; }
    public bool HasRule { get; init; }
    public string RuleState { get; init; } = string.Empty;
}
