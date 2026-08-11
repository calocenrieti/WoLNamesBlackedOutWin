using Microsoft.UI.Xaml;
using System;
using System.Globalization;
using System.Runtime.ExceptionServices;
using System.IO;
using System.Collections.Generic;
using System.Linq;
using System.Threading;
using Windows.Globalization;
using Windows.Storage;
using Windows.ApplicationModel;
using Windows.System.UserProfile;

namespace WoLNamesBlackedOut
{
    public partial class App : Application
    {
        private const string UiLanguagePreferenceKey = "LanguageJP";
        private static readonly string CrashLogFileName = CreateTimestampedFileName("wol_unhandled_exception.log");

        public static MainWindow MainWindow { get; private set; } = null!;

        public App()
        {
            WriteCrashLog("startup", "App.ctor begin");
            ApplySavedUiCulturePreference();
            this.InitializeComponent();
            WriteCrashLog("startup", "App.ctor after InitializeComponent");
            this.UnhandledException += App_UnhandledException;
            AppDomain.CurrentDomain.UnhandledException += CurrentDomain_UnhandledException;
            AppDomain.CurrentDomain.FirstChanceException += CurrentDomain_FirstChanceException;
            WriteCrashLog("startup", "App.ctor end");
        }

        private static void ApplySavedUiCulturePreference()
        {
            bool useJapanese = IsWindowsJapaneseEnvironment();
            bool hasSavedPreference = false;

            try
            {
                var localSettings = ApplicationData.Current.LocalSettings;
                if (localSettings.Values.TryGetValue(UiLanguagePreferenceKey, out object value) && bool.TryParse(value?.ToString(), out bool parsed))
                {
                    hasSavedPreference = true;
                    useJapanese = parsed;
                }

                if (!hasSavedPreference)
                {
                    localSettings.Values[UiLanguagePreferenceKey] = useJapanese;
                }
            }
            catch
            {
            }

            try
            {
                ApplicationLanguages.PrimaryLanguageOverride = useJapanese ? "ja-JP" : "en-US";
            }
            catch
            {
            }

            var culture = new CultureInfo(useJapanese ? "ja-JP" : "en-US");
            CultureInfo.DefaultThreadCurrentCulture = culture;
            CultureInfo.DefaultThreadCurrentUICulture = culture;
            Thread.CurrentThread.CurrentCulture = culture;
            Thread.CurrentThread.CurrentUICulture = culture;
        }

        private static bool IsWindowsJapaneseEnvironment()
        {
            try
            {
                string? firstLanguage = GlobalizationPreferences.Languages?.FirstOrDefault();
                if (!string.IsNullOrWhiteSpace(firstLanguage))
                {
                    return firstLanguage.StartsWith("ja", StringComparison.OrdinalIgnoreCase);
                }
            }
            catch
            {
            }

            return string.Equals(CultureInfo.CurrentUICulture.TwoLetterISOLanguageName, "ja", StringComparison.OrdinalIgnoreCase);
        }

        private static void WriteCrashLog(string title, string details)
        {
            foreach (var path in GetLogPaths(CrashLogFileName))
            {
                try
                {
                    File.AppendAllText(path, $"[{DateTime.Now:O}] {title}{Environment.NewLine}{details}{Environment.NewLine}{Environment.NewLine}");
                }
                catch
                {
                }
            }
        }

        private static IEnumerable<string> GetLogPaths(string fileName)
        {
            var paths = new List<string>();

            try
            {
                paths.Add(Path.Combine(Path.GetTempPath(), fileName));
            }
            catch
            {
            }

            try
            {
                paths.Add(Path.Combine(ApplicationData.Current.LocalFolder.Path, fileName));
            }
            catch
            {
            }

            return paths;
        }

        private static string CreateTimestampedFileName(string fileName)
        {
            string extension = Path.HasExtension(fileName) ? Path.GetExtension(fileName) : string.Empty;
            string nameWithoutExtension = Path.HasExtension(fileName) ? Path.GetFileNameWithoutExtension(fileName) : fileName;
            return $"{nameWithoutExtension}_{DateTime.Now:yyyyMMdd_HHmmssfff}{extension}";
        }

        private void App_UnhandledException(object sender, Microsoft.UI.Xaml.UnhandledExceptionEventArgs e)
        {
            string details = e.Exception?.ToString() ?? "(null exception)";
            try
            {
                details = $"Package={Package.Current?.Id?.FullName}{Environment.NewLine}{details}";
            }
            catch
            {
            }
            WriteCrashLog("App.UnhandledException", details);
            e.Handled = true;
        }

        private static void CurrentDomain_UnhandledException(object sender, System.UnhandledExceptionEventArgs e)
        {
            string details = e.ExceptionObject?.ToString() ?? "(null exception object)";
            WriteCrashLog("AppDomain.CurrentDomain.UnhandledException", details);
        }

        private static void CurrentDomain_FirstChanceException(object? sender, FirstChanceExceptionEventArgs e)
        {
            if (e.Exception is InvalidOperationException)
            {
                WriteCrashLog("AppDomain.CurrentDomain.FirstChanceException", e.Exception.ToString());
            }
        }

        protected override void OnLaunched(Microsoft.UI.Xaml.LaunchActivatedEventArgs args)
        {
            WriteCrashLog("startup", "OnLaunched begin");
            MainWindow = new MainWindow();
            WriteCrashLog("startup", "OnLaunched after new MainWindow");
            MainWindow.Activate();
            WriteCrashLog("startup", "OnLaunched after Activate");
        }
    }
}
