using System.Text.Json.Serialization;

namespace WinAnanicyGui
{
    /// <summary>
    /// Represents a process optimization rule defined in rules.json
    /// </summary>
    public class ProcessRule
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
        public bool? BackgroundOnly { get; set; }

        [JsonPropertyName("eco_qos")]
        public bool? EcoQoS { get; set; }

        [JsonPropertyName("launcher")]
        public bool? Launcher { get; set; }

        [JsonPropertyName("cpu_limit")]
        public int? CpuLimit { get; set; }

        /// <summary>
        /// Clone helper to copy rules when editing
        /// </summary>
        public ProcessRule Clone()
        {
            return new ProcessRule
            {
                ProcessName = this.ProcessName,
                CpuPriority = this.CpuPriority,
                IoPriority = this.IoPriority,
                CpuAffinity = this.CpuAffinity,
                BackgroundOnly = this.BackgroundOnly,
                EcoQoS = this.EcoQoS,
                Launcher = this.Launcher,
                CpuLimit = this.CpuLimit
            };
        }
    }
}
