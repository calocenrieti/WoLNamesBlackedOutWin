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

        [STAThread]
        private static void Main(string[] args)
        {
            WriteBootstrapLog("main:begin");

            TryApplyForceCpuPipelineAtProcessStart();

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

        private static void TryApplyForceCpuPipelineAtProcessStart()
        {
            try
            {
                bool forceCpu = false;
                var settings = ApplicationData.Current.LocalSettings;
                if (settings != null && settings.Values.TryGetValue(ForceCpuPipelinePreferenceKey, out object value))
                {
                    _ = bool.TryParse(value?.ToString(), out forceCpu);
                }

                Environment.SetEnvironmentVariable("WOL_FORCE_CPU_PIPELINE", forceCpu ? "1" : "0", EnvironmentVariableTarget.Process);
                WriteBootstrapLog($"main:force_cpu_pipeline={(forceCpu ? "true" : "false")}");
            }
            catch (Exception ex)
            {
                WriteBootstrapLog($"main:force_cpu_pipeline_read_failed:{ex.Message}");
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
