using Microsoft.UI.Xaml;
using System;
using System.IO;

namespace WoLNamesBlackedOut
{
    public static class Program
    {
        private static readonly string BootstrapLogFileName = CreateTimestampedFileName("wol_bootstrap.log");

        [STAThread]
        private static void Main(string[] args)
        {
            WriteBootstrapLog("main:begin");

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

        private static void WriteBootstrapLog(string marker)
        {
            string line = $"[{DateTime.Now:O}] {marker}{Environment.NewLine}";

            TryAppend(Path.Combine(Path.GetTempPath(), BootstrapLogFileName), line);

            try
            {
                string localAppData = Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData);
                if (!string.IsNullOrWhiteSpace(localAppData))
                {
                    TryAppend(Path.Combine(localAppData, BootstrapLogFileName), line);
                }
            }
            catch
            {
            }
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
