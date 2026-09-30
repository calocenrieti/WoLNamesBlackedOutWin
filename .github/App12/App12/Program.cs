using Microsoft.UI.Xaml;
using System;
using System.IO;
using Windows.Storage;

namespace WoLNamesBlackedOut
{
    public static class Program
    {
        private static readonly string BootstrapLogFileName = CreateTimestampedFileName("wol_bootstrap.log");
        private const string ForceCpuPipelinePreferenceKey = "ForceCpuPipeline";
        private const string DisableDgpuPipelinePreferenceKey = "DisableDgpuPipeline";
        private const string StrictIGpuOnlyPreferenceKey = "StrictIGpuOnly";

        [STAThread]
        private static void Main(string[] args)
        {
            WriteBootstrapLog("main:begin");

            TryApplyPipelineModesAtProcessStart();

            AppDomain.CurrentDomain.FirstChanceException += (s, e) =>
            {
                if (e.Exception is AccessViolationException || e.Exception is System.Runtime.InteropServices.SEHException)
                {
                    WriteBootstrapLog($"firstchance:{e.Exception.GetType().Name}:{e.Exception.Message}");
                }
            };

            AppDomain.CurrentDomain.UnhandledException += (s, e) =>
            {
                var text = e.ExceptionObject?.ToString() ?? "(null)";
                WriteBootstrapLog($"unhandled:{text}");
            };

            try
            {
                WinRT.ComWrappersSupport.InitializeComWrappers();
                WriteBootstrapLog("main:after InitializeComWrappers");

                Application.Start((p) =>
                {
                    WriteBootstrapLog("main:Application.Start callback begin");
                    var context = new Microsoft.UI.Dispatching.DispatcherQueueSynchronizationContext(
                        Microsoft.UI.Dispatching.DispatcherQueue.GetForCurrentThread());
                    System.Threading.SynchronizationContext.SetSynchronizationContext(context);
                    WriteBootstrapLog("main:before new App");
                    _ = new App();
                    WriteBootstrapLog("main:after new App");
                });

                WriteBootstrapLog("main:Application.Start return");
            }
            catch (Exception ex)
            {
                WriteBootstrapLog($"main:catch:{ex}");
                throw;
            }
        }

        private static void TryApplyPipelineModesAtProcessStart()
        {
            try
            {
                bool forceCpu = false;
                bool disableDgpu = false;
                bool strictIGpuOnly = false;
                var settings = ApplicationData.Current.LocalSettings;
                if (settings != null && settings.Values.TryGetValue(ForceCpuPipelinePreferenceKey, out object value))
                {
                    _ = bool.TryParse(value?.ToString(), out forceCpu);
                }

                if (settings != null && settings.Values.TryGetValue(DisableDgpuPipelinePreferenceKey, out object disableDgpuValue))
                {
                    _ = bool.TryParse(disableDgpuValue?.ToString(), out disableDgpu);
                }

                if (settings != null && settings.Values.TryGetValue(StrictIGpuOnlyPreferenceKey, out object strictIGpuOnlyValue))
                {
                    _ = bool.TryParse(strictIGpuOnlyValue?.ToString(), out strictIGpuOnly);
                }

                if (forceCpu)
                {
                    disableDgpu = false;
                    strictIGpuOnly = false;
                }
                else if (strictIGpuOnly)
                {
                    disableDgpu = false;
                }
                else if (disableDgpu)
                {
                    strictIGpuOnly = false;
                }

                Environment.SetEnvironmentVariable("WOL_FORCE_CPU_PIPELINE", forceCpu ? "1" : "0", EnvironmentVariableTarget.Process);
                Environment.SetEnvironmentVariable("WOL_DISABLE_DGPU", disableDgpu ? "1" : "0", EnvironmentVariableTarget.Process);
                Environment.SetEnvironmentVariable("WOL_STRICT_IGPU_ONLY", strictIGpuOnly ? "1" : "0", EnvironmentVariableTarget.Process);
                WriteBootstrapLog($"main:force_cpu_pipeline={(forceCpu ? "true" : "false")},disable_dgpu={(disableDgpu ? "true" : "false")},strict_igpu_only={(strictIGpuOnly ? "true" : "false")}");
            }
            catch (Exception ex)
            {
                WriteBootstrapLog($"main:pipeline_mode_read_failed:{ex.Message}");
            }
        }

        private static void WriteBootstrapLog(string marker)
        {
            string line = $"[{DateTime.Now:O}] {marker}{Environment.NewLine}";

            TryAppend(Path.Combine(Path.GetTempPath(), BootstrapLogFileName), line);
        }

        private static string CreateTimestampedFileName(string fileName)
        {
            string extension = Path.HasExtension(fileName) ? Path.GetExtension(fileName) : string.Empty;
            string nameWithoutExtension = Path.HasExtension(fileName) ? Path.GetFileNameWithoutExtension(fileName) : fileName;
            return $"{nameWithoutExtension}_{DateTime.Now:yyyyMMdd_HHmmssfff}{extension}";
        }

        private static void TryAppend(string path, string line)
        {
            try
            {
                File.AppendAllText(path, line);
            }
            catch
            {
            }
        }
    }
}
