using Microsoft.UI;
using Microsoft.UI.Windowing;
using Microsoft.UI.Composition.SystemBackdrops;
using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;
using Microsoft.UI.Xaml.Controls.Primitives;
using Microsoft.UI.Xaml.Input;
using Microsoft.UI.Xaml.Media;
using Microsoft.UI.Xaml.Media.Imaging;
using Microsoft.UI.Xaml.Shapes;
using Microsoft.UI.Xaml.Data;
using System;
using System.Collections.Generic;
using System.Globalization;
using System.Diagnostics;
using System.Globalization;
using System.IO;
using System.Linq;
using System.Runtime.InteropServices;
using System.Runtime.InteropServices.WindowsRuntime;
using System.Text;
using System.Threading;
using System.Threading.Tasks;
using Windows.ApplicationModel;
using Windows.ApplicationModel.Resources;
using Windows.ApplicationModel.DataTransfer;
using Windows.Globalization;
using Windows.Graphics.Imaging;
using Windows.Storage;
using Windows.Storage.Pickers;
using Windows.UI;
using WinRT.Interop;

internal static class WindowHelper
{
    [DllImport("User32.dll")]
    private static extern int GetDpiForWindow(nint hwnd);

    public const double DefaultPixelsPerInch = 96D;

    public static double GetWindowDpiScale(Window window)
    {
        nint windowHandle = WinRT.Interop.WindowNative.GetWindowHandle(window);
        return GetDpiForWindow(windowHandle) / DefaultPixelsPerInch;
    }
}

namespace WoLNamesBlackedOut
{
    public sealed class TwoDecimalSliderToolTipConverter : IValueConverter
    {
        public object Convert(object value, Type targetType, object parameter, string language)
        {
            if (value is double d)
            {
                return d.ToString("F2", CultureInfo.InvariantCulture);
            }

            if (value is float f)
            {
                return f.ToString("F2", CultureInfo.InvariantCulture);
            }

            return "0.00";
        }

        public object ConvertBack(object value, Type targetType, object parameter, string language)
        {
            if (value is string s && double.TryParse(s, NumberStyles.Float, CultureInfo.InvariantCulture, out double d))
            {
                return d;
            }

            return 0.0;
        }
    }

    public sealed partial class MainWindow : Microsoft.UI.Xaml.Window
    {
        private int v_fps;
        private int v_width;
        private int v_height;
        private int v_nb_frames;
        private string v_color_primaries;
        private string v_file_path;
        private int v_start_time;
        private int v_end_time;
        private string ffmpegPath = System.IO.Path.Combine(AppContext.BaseDirectory, "ffmpeg.exe");
        private Windows.Foundation.Point startPoint;
        private Rectangle currentRectangle;
        private bool isDrawing = false; // 矩形描画中かどうかを示すフラグ
        private List<Windows.Foundation.Rect> savedRects = []; // 保存された矩形の座標を保持するためのフィールド
        private string last_preview_image;
        private double scaleFactor;
        private string codec;
        private string hwaccel;
        private string preset;
        private bool running_state = false; // 実行中かどうかを示すフラグ
        private bool cancel_state = false; // キャンセルかどうかを示すフラグ
        private bool cancel_pending_state = false; // キャンセル確定待ちかどうか
        private bool v_hasAudio = true; // 音声があるかどうかを示すフラグ
        private bool useblackedout = true;
        private bool isWindowClosing = false;
        private bool suppressPreviewToggleEvent = false;
        private bool suppressBlackedOutToggleEvent = false;
        private bool suppressPreviewFrameSliderRefresh = false;

        private bool excludeByNameEnabled = false;
        private int ocrExpandPixels = 2;
        private int ocrMaxRoisPerFrame = 6;
        private int textSimilarityPercent = 85;
        private string maskExcludeTextCsv = string.Empty;
        private const string UiLanguagePreferenceKey = "LanguageJP";

        private static void ApplyUiCulture(bool useJapanese)
        {
            var culture = new CultureInfo(useJapanese ? "ja-JP" : "en-US");
            CultureInfo.DefaultThreadCurrentCulture = culture;
            CultureInfo.DefaultThreadCurrentUICulture = culture;
            Thread.CurrentThread.CurrentCulture = culture;
            Thread.CurrentThread.CurrentUICulture = culture;
        }

        private bool IsJapaneseUiCulture()
        {
            var lang = CultureInfo.CurrentUICulture.TwoLetterISOLanguageName;
            return string.Equals(lang, "ja", StringComparison.OrdinalIgnoreCase);
        }

        private static string GetLocalizedString(string key, string fallback)
        {
            try
            {
                string value = ResourceLoader.GetForViewIndependentUse().GetString(key);
                return string.IsNullOrWhiteSpace(value) ? fallback : value;
            }
            catch
            {
                return fallback;
            }
        }

        private readonly MicaBackdrop windowMicaBackdrop = new MicaBackdrop();


        private Microsoft.UI.Xaml.DispatcherTimer timer;
    private Microsoft.UI.Xaml.DispatcherTimer previewTimer;
        private Microsoft.UI.Xaml.DispatcherTimer previewStatusTimer;
        private Stopwatch stopwatch;
    private bool previewSessionOpen = false;
    private bool previewTickBusy = false;
        private int previewBusyIndicatorCount = 0;
    private int previewFrameIndex = 0;
    private int previewFrameStep = 1;
    private bool previewThresholdUpdateBusy = false;
    private bool suppressFrameSliderValueChanged = false;
        private bool previewSessionAutoOpenBusy = false;
    private bool imagePreviewRefreshBusy = false;
    private WriteableBitmap? previewBitmap;
    private bool previewLayoutInitialized = false;
        private readonly object previewFrameCacheLock = new object();
        private byte[]? lastPreviewFrameBgra;
        private int lastPreviewFrameWidth;
        private int lastPreviewFrameHeight;

        // Copyright overlay interaction state
        private int copyrightOffsetX = 0;
        private int copyrightOffsetY = 0;
        private bool copyrightDragging = false;
        private double copyrightDragStartX = 0;
        private double copyrightDragStartY = 0;
        private double copyrightZoomScale = 1.0;
        private string? copyrightImagePath = null;
        private static readonly TimeSpan ProgressIndicatorLeadTime = TimeSpan.FromMilliseconds(120);

        private void ClearPreviewFrameCache()
        {
            lock (previewFrameCacheLock)
            {
                lastPreviewFrameBgra = null;
                lastPreviewFrameWidth = 0;
                lastPreviewFrameHeight = 0;
            }
        }

        private async Task WaitForProgressIndicatorAsync()
        {
            await Task.Yield();
            await Task.Delay(ProgressIndicatorLeadTime);
        }

        private bool IsCurrentSourceImageFile()
        {
            if (string.IsNullOrWhiteSpace(v_file_path))
            {
                return false;
            }

            string ext = System.IO.Path.GetExtension(v_file_path).ToLowerInvariant();
            return ext == ".jpg" || ext == ".jpeg" || ext == ".png";
        }

        private async Task RefreshCurrentImagePreviewAsync()
        {
            if (isWindowClosing || previewSessionOpen || imagePreviewRefreshBusy || !IsCurrentSourceImageFile() || !File.Exists(v_file_path))
            {
                return;
            }

            imagePreviewRefreshBusy = true;
            SafeSetProgressIndeterminate(true);

            string tempDirectory = System.IO.Path.GetTempPath();
            string fileName = $"tmp_wol_{DateTime.Now:yyyyMMddHHmmssfff}.png";
            string outputPath = System.IO.Path.Combine(tempDirectory, fileName);

            try
            {
                File.Copy(v_file_path, outputPath, true);

                RectInfo[] rectInfos = BuildRectInfos();
                var (nameColor, fixedColor) = BuildMaskColors();

                int result = await FrameProcessor.Runpreview_apiAsync(
                    outputPath,
                    rectInfos,
                    rectInfos.Length,
                    nameColor,
                    fixedColor,
                    Add_Copyright.IsChecked == true,
                    GetComboText(BlackedOut_ComboBox, "Solid"),
                    GetComboText(FixedFrame_ComboBox, "Solid"),
                    (int)BlackedOutSlideBar.Value,
                    (int)FixedFrameSlideBar.Value,
                    GetYoloThreshold(),
                    copyrightImagePath ?? string.Empty);

                if (result != 0 || isWindowClosing)
                {
                    return;
                }

                last_preview_image = outputPath;
                var bitmapImage = new BitmapImage(new Uri(outputPath));
                bitmapImage.ImageOpened += (s, ev) =>
                {
                    if (isWindowClosing)
                    {
                        return;
                    }

                    var originalHeight = bitmapImage.PixelHeight;
                    scaleFactor = image_preview.Height / originalHeight;

                    int logicalWidth = 800 + (int)(bitmapImage.PixelWidth * scaleFactor) - 280;
                    int logicalHeight = 850;
                    TryResizeAppWindow(logicalWidth, logicalHeight);

                    image_preview.Source = bitmapImage;
                    RemoveAllRectangles();
                };

                image_preview.Source = bitmapImage;
                SaveImageButton.IsEnabled = true;
            }
            finally
            {
                imagePreviewRefreshBusy = false;
                SafeSetProgressIndeterminate(false);
            }
        }

        private void SetPreviewBusyIndicator(bool busy)
        {
            if (ProgressBar == null || isWindowClosing)
            {
                return;
            }

            if (busy)
            {
                previewBusyIndicatorCount++;
            }
            else
            {
                previewBusyIndicatorCount = Math.Max(0, previewBusyIndicatorCount - 1);
            }

            try
            {
                ProgressBar.IsIndeterminate = previewBusyIndicatorCount > 0;
            }
            catch (ArgumentException)
            {
                isWindowClosing = true;
            }
            catch (COMException)
            {
                isWindowClosing = true;
            }
        }

        private void SafeSetProgressIndeterminate(bool isIndeterminate)
        {
            if (ProgressBar == null || isWindowClosing)
            {
                return;
            }

            try
            {
                ProgressBar.IsIndeterminate = isIndeterminate;
            }
            catch (ArgumentException)
            {
                isWindowClosing = true;
            }
            catch (COMException)
            {
                isWindowClosing = true;
            }
        }

        private void BeginWindowClosing()
        {
            if (isWindowClosing)
            {
                return;
            }

            isWindowClosing = true;
            previewBusyIndicatorCount = 0;

            try
            {
                timer?.Stop();
                previewTimer?.Stop();
                previewStatusTimer?.Stop();
            }
            catch
            {
            }

            try
            {
                StopRealtimePreviewSession(skipNativeClose: true);
            }
            catch
            {
            }

            try
            {
                SafeSetProgressIndeterminate(false);
            }
            catch
            {
            }
        }

        private void PreviewStatusTimer_Tick(object? sender, object e)
        {
            if (isWindowClosing)
            {
                return;
            }

            var latestStatus = FrameProcessor.LatestStatusMessage;
            if (!string.IsNullOrWhiteSpace(latestStatus))
            {
                FFMpeg_text.Text = latestStatus;
            }
        }

        private void BeginPreviewStatusFeedback(string initialMessage)
        {
            FrameProcessor.PrepareStatusTracking();
            if (!string.IsNullOrWhiteSpace(initialMessage))
            {
                FFMpeg_text.Text = initialMessage;
            }
            previewStatusTimer?.Start();
        }

        private void EndPreviewStatusFeedback(string fallbackMessage = "")
        {
            previewStatusTimer?.Stop();

            var latestStatus = FrameProcessor.LatestStatusMessage;
            if (!string.IsNullOrWhiteSpace(latestStatus))
            {
                FFMpeg_text.Text = latestStatus;
            }
            else if (!string.IsNullOrWhiteSpace(fallbackMessage))
            {
                FFMpeg_text.Text = fallbackMessage;
            }
        }

        // C++ の構造体に対応する C# の構造体を定義
        [StructLayout(LayoutKind.Sequential)]
        public struct RectInfo
        {
            public int x;
            public int y;
            public int width;
            public int height;
        }

        [StructLayout(LayoutKind.Sequential)]
        public struct ColorInfo
        {
            public byte r;
            public byte g;
            public byte b;
        }

        public enum MaskTypeKind
        {
            Solid = 3,
            Mosaic = 1,
            Blur = 2,
            Inpaint = 0,
            NoInference = 4,
        }

        [DllImport("WoLNamesBlackedOut_DLL.dll", CallingConvention = CallingConvention.StdCall)]
        private static extern char GetGpuVendor();

        private static char SafeGetGpuVendor()
        {
            try
            {
                return GetGpuVendor();
            }
            catch (AccessViolationException ex)
            {
                Debug.WriteLine($"GetGpuVendor access violation: {ex.Message}");
                StartupTrace($"native:GetGpuVendor access violation {ex.Message}");
                return 'X';
            }
            catch (SEHException ex)
            {
                Debug.WriteLine($"GetGpuVendor SEH exception: {ex.Message}");
                StartupTrace($"native:GetGpuVendor SEH exception {ex.Message}");
                return 'X';
            }
            catch (BadImageFormatException ex)
            {
                Debug.WriteLine($"GetGpuVendor bad image format: {ex.Message}");
                StartupTrace($"native:GetGpuVendor bad image format {ex.Message}");
                return 'X';
            }
            catch (DllNotFoundException ex)
            {
                Debug.WriteLine($"GetGpuVendor DLL missing: {ex.Message}");
                return 'X';
            }
            catch (EntryPointNotFoundException ex)
            {
                Debug.WriteLine($"GetGpuVendor entry point missing: {ex.Message}");
                return 'X';
            }
            catch (Exception ex)
            {
                Debug.WriteLine($"GetGpuVendor unexpected exception: {ex}");
                StartupTrace($"native:GetGpuVendor unexpected {ex.GetType().Name}: {ex.Message}");
                return 'X';
            }
        }

        private static int SafeGetTotalFrameCount()
        {
            return FrameProcessor.LatestProcessedFrames;
        }

        private static bool SafeCancelFfmpegProcesses()
        {
            return FrameProcessor.TryCancelProcess();
        }

        private enum OptimizationProfile
        {
            Safe = 0,
            Step1GpuOnly = 1,
            Step2IoBindingOnly = 2,
            Step3Both = 3,
        }

        private OptimizationProfile GetSelectedOptimizationProfile()
        {
            int selectedIndex = OptimizationModeComboBox?.SelectedIndex ?? 3;
            return selectedIndex switch
            {
                1 => OptimizationProfile.Step1GpuOnly,
                2 => OptimizationProfile.Step2IoBindingOnly,
                3 => OptimizationProfile.Step3Both,
                _ => OptimizationProfile.Step3Both,
            };
        }

        private static string ToEnvBool(bool value)
        {
            return value ? "1" : "0";
        }

        private string ApplyOptimizationProfileToEnvironment()
        {
            var profile = GetSelectedOptimizationProfile();

            bool useGpuPreprocess = profile is OptimizationProfile.Step1GpuOnly or OptimizationProfile.Step3Both;
            bool useIoBinding = profile is OptimizationProfile.Step2IoBindingOnly or OptimizationProfile.Step3Both;

            string gpuValue = ToEnvBool(useGpuPreprocess);
            string ioValue = ToEnvBool(useIoBinding);

            Environment.SetEnvironmentVariable("WOL_USE_GPU_PREPROCESS", gpuValue, EnvironmentVariableTarget.Process);
            Environment.SetEnvironmentVariable("WOL_USE_IOBINDING", ioValue, EnvironmentVariableTarget.Process);

            // Keep user-level values aligned so native code can read stable toggles even across process boundaries.
            Environment.SetEnvironmentVariable("WOL_USE_GPU_PREPROCESS", gpuValue, EnvironmentVariableTarget.User);
            Environment.SetEnvironmentVariable("WOL_USE_IOBINDING", ioValue, EnvironmentVariableTarget.User);

            string modeLabel = profile switch
            {
                OptimizationProfile.Step1GpuOnly => "Step1(GPU On / IoBinding Off)",
                OptimizationProfile.Step2IoBindingOnly => "Step2(GPU Off / IoBinding On)",
                OptimizationProfile.Step3Both => "Step3(GPU On / IoBinding On)",
                _ => "Safe(GPU Off / IoBinding Off)",
            };

            Debug.WriteLine($"Optimization profile: {modeLabel}");
            return modeLabel;
        }

        private static Windows.Storage.ApplicationDataContainer? TryGetLocalSettings()
        {
            try
            {
                return Windows.Storage.ApplicationData.Current.LocalSettings;
            }
            catch (Exception ex)
            {
                Debug.WriteLine($"LocalSettings unavailable: {ex.Message}");
                return null;
            }
        }

        private static string GetAppDataRootPath()
        {
            try
            {
                return Windows.Storage.ApplicationData.Current.LocalFolder.Path;
            }
            catch (Exception ex)
            {
                Debug.WriteLine($"LocalFolder unavailable, fallback to LocalApplicationData: {ex.Message}");
                return Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData);
            }
        }

        private static readonly object debugLogFilePathLock = new object();
        private static string? debugLogFilePath;

        private static bool IsDebugLogExportEnabled()
        {
            string? value = Environment.GetEnvironmentVariable("WOL_DEBUG_LOG_EXPORT", EnvironmentVariableTarget.Process);
            if (string.IsNullOrWhiteSpace(value))
            {
                return false;
            }

            value = value.Trim();
            return value == "1" || value.Equals("true", StringComparison.OrdinalIgnoreCase);
        }

        private static string GetOrCreateDebugLogPath()
        {
            lock (debugLogFilePathLock)
            {
                if (!string.IsNullOrEmpty(debugLogFilePath))
                {
                    return debugLogFilePath;
                }

                string fileName = $"wol_DebugLog_{DateTime.Now:yyyyMMdd_HHmmss}.txt";
                debugLogFilePath = System.IO.Path.Combine(System.IO.Path.GetTempPath(), fileName);
                return debugLogFilePath;
            }
        }

        private static void AppendDebugLog(string marker)
        {
            if (!IsDebugLogExportEnabled())
            {
                return;
            }

            try
            {
                string logPath = GetOrCreateDebugLogPath();
                File.AppendAllText(logPath, $"{DateTime.Now:O} {marker}{Environment.NewLine}");
            }
            catch
            {
            }
        }

        private void DebugLogExport_Click(object sender, RoutedEventArgs e)
        {
            bool enabled = DebugLogExport?.IsChecked == true;
            Environment.SetEnvironmentVariable("WOL_DEBUG_LOG_EXPORT", enabled ? "1" : "0", EnvironmentVariableTarget.Process);
            AppendDebugLog($"DebugLogExport={(enabled ? "true" : "false")}");
        }

        private void LanguageJP_Click(object sender, RoutedEventArgs e)
        {
            bool useJapanese = LanguageJP?.IsChecked == true;

            var localSettings = TryGetLocalSettings();
            if (localSettings != null)
            {
                try { localSettings.Values[UiLanguagePreferenceKey] = useJapanese; } catch { }
            }

            try
            {
                ApplicationLanguages.PrimaryLanguageOverride = useJapanese ? "ja-JP" : "en-US";
            }
            catch
            {
            }

            ApplyUiCulture(useJapanese);
        }

        private static void StartupTrace(string marker)
        {
            AppendDebugLog($"[StartupTrace] {marker}");
        }

        public MainWindow()
        {
            StartupTrace("ctor:start");
            this.InitializeComponent();
            StartupTrace("ctor:after init");

            TryEnableMicaBackdrop();

            try
            {
                ExtendsContentIntoTitleBar = true;
                this.SetTitleBar(DragRegion);

                // Set the preferred height option for the title bar
                this.AppWindow.TitleBar.PreferredHeightOption = TitleBarHeightOption.Collapsed;
            }
            catch (Exception ex)
            {
                Debug.WriteLine($"Title bar initialization failed: {ex}");
                ExtendsContentIntoTitleBar = false;
            }

            try
            {
                // DPIスケールを取得
                double dpiScale = WindowHelper.GetWindowDpiScale(this);

                // 論理サイズをDPIスケールで変換
                int logicalWidth = 850;
                int logicalHeight = 850;
                int physicalWidth = (int)(logicalWidth * dpiScale);
                int physicalHeight = (int)(logicalHeight * dpiScale);

                var initialSize = new Windows.Graphics.SizeInt32
                {
                    Width = physicalWidth,
                    Height = physicalHeight
                };
                this.AppWindow.Resize(initialSize);
            }
            catch (Exception ex)
            {
                // Resize が失敗することは稀ですが、デバッグ用にログを残します
                Debug.WriteLine($"AppWindow.Resize failed: {ex.Message}");
            }

            try
            {
                this.AppWindow.Closing += AppWindow_Closing;
            }
            catch
            {
            }

            RootGrid.KeyDown += MainWindow_KeyDown;
            RootGrid.Loaded += (_, __) => RootGrid.Focus(FocusState.Programmatic);

            StartupTrace("ctor:after event hooks");

            UIControl_enable_false();
            PickAFileButton.IsEnabled = true;


            // LocalSettings から値を読み込む
            var localSettings = TryGetLocalSettings();

            bool useJapaneseUi = IsJapaneseUiCulture();
            if (localSettings != null && localSettings.Values.TryGetValue(UiLanguagePreferenceKey, out object languagePreferenceValue))
            {
                _ = bool.TryParse(languagePreferenceValue?.ToString(), out useJapaneseUi);
            }

            LanguageJP.IsChecked = useJapaneseUi;

            try
            {
                ApplicationLanguages.PrimaryLanguageOverride = useJapaneseUi ? "ja-JP" : "en-US";
            }
            catch
            {
            }

            ApplyUiCulture(useJapaneseUi);

            // Add_Copyright の設定を読み込み
            if (localSettings != null && localSettings.Values.TryGetValue("Add_Copyright", out object addCopyrightValue))
            {
                if (bool.TryParse(addCopyrightValue.ToString(), out bool isChecked))
                {
                    Add_Copyright.IsChecked = isChecked;
                }
            }

            // BitrateSlideBar の値を読み込み、反映
            if (localSettings != null && localSettings.Values.TryGetValue("Bitrate", out object bitrateValue))
            {
                if (double.TryParse(bitrateValue.ToString(), out double sliderValue))
                {
                    BitrateSlideBar.Value = sliderValue;
                }
            }
            // YoloThresholdSlider の値を読み込み、反映
            if (localSettings != null && localSettings.Values.TryGetValue("YoloThresholdSlider", out object yoloThresholdValue))
            {
                if (double.TryParse(yoloThresholdValue.ToString(), out double sliderValue))
                {
                    YoloThresholdSlider.Value = sliderValue;
                }
            }

            if (localSettings != null && localSettings.Values.TryGetValue("ExcludeByNameEnabled", out object excludeByNameEnabledValue))
            {
                _ = bool.TryParse(excludeByNameEnabledValue?.ToString(), out excludeByNameEnabled);
            }

            if (localSettings != null && localSettings.Values.TryGetValue("OcrExpandPixels", out object ocrExpandPixelsValue))
            {
                if (int.TryParse(ocrExpandPixelsValue?.ToString(), out int parsedOcrExpandPixels))
                {
                    ocrExpandPixels = Math.Clamp(parsedOcrExpandPixels, 0, 5);
                }
            }

            if (localSettings != null && localSettings.Values.TryGetValue("OcrMaxRoisPerFrame", out object ocrMaxRoisPerFrameValue))
            {
                if (int.TryParse(ocrMaxRoisPerFrameValue?.ToString(), out int parsedOcrMaxRoisPerFrame))
                {
                    ocrMaxRoisPerFrame = Math.Clamp(parsedOcrMaxRoisPerFrame, 1, 32);
                }
            }

            if (localSettings != null && localSettings.Values.TryGetValue("TextSimilarityPercent", out object textSimilarityPercentValue))
            {
                if (int.TryParse(textSimilarityPercentValue?.ToString(), out int parsedTextSimilarityPercent))
                {
                    textSimilarityPercent = Math.Clamp(parsedTextSimilarityPercent, 50, 100);
                }
            }

            if (localSettings != null && localSettings.Values.TryGetValue("MaskExcludeTextCsv", out object maskExcludeTextCsvValue))
            {
                maskExcludeTextCsv = maskExcludeTextCsvValue?.ToString() ?? string.Empty;
            }

            ExcludeByNameEnabledCheckBox.IsChecked = excludeByNameEnabled;
            OpenExcludeByNameSettingsButton.IsEnabled = excludeByNameEnabled;

            // themeValue の値を読み込み、反映
            if (localSettings != null && localSettings.Values.TryGetValue("AppTheme", out object themeValue))
            {
                if (themeValue.ToString() == "Dark")
                {
                    ApplyTheme(ElementTheme.Dark);
                }
                else
                {
                    ApplyTheme(ElementTheme.Light);
                }
            }

            bool debugLogExportEnabled = false;
            if (localSettings != null && localSettings.Values.TryGetValue("DebugLogExport", out object debugLogExportValue))
            {
                _ = bool.TryParse(debugLogExportValue?.ToString(), out debugLogExportEnabled);
            }

            DebugLogExport.IsChecked = debugLogExportEnabled;
            Environment.SetEnvironmentVariable("WOL_DEBUG_LOG_EXPORT", debugLogExportEnabled ? "1" : "0", EnvironmentVariableTarget.Process);
            DebugLogExport.Click += DebugLogExport_Click;

            // 最適化プロファイルは常にStep3（UIは非表示）
            OptimizationModeComboBox.SelectedIndex = 3;
            ConfigureMaskSliderForType(BlackedOutSlideBar, GetComboText(BlackedOut_ComboBox, "Solid"));
            ConfigureMaskSliderForType(FixedFrameSlideBar, GetComboText(FixedFrame_ComboBox, "Solid"));
            StartupTrace("ctor:after settings");

            // AppDataのパスを取得
            string localAppDataPath = GetAppDataRootPath();
            StartupTrace("ctor:after appdata path");

            // アプリケーション専用のフォルダを作成
            string appFolder = System.IO.Path.Combine(localAppDataPath, "WoLNamesBlackedOut");
            if (!Directory.Exists(appFolder))
            {
                Directory.CreateDirectory(appFolder);
                Console.WriteLine($"Folder created successfully: {appFolder}");
            }
            else
            {
                Console.WriteLine($"Folder already exists: {appFolder}");
            }

            stopwatch = new Stopwatch();
            timer = new Microsoft.UI.Xaml.DispatcherTimer
            {
                Interval = TimeSpan.FromMilliseconds(100)
            };
            timer.Tick += Timer_Tick;

            previewTimer = new Microsoft.UI.Xaml.DispatcherTimer
            {
                Interval = TimeSpan.FromMilliseconds(25)
            };
            previewTimer.Tick += PreviewTimer_Tick;

            previewStatusTimer = new Microsoft.UI.Xaml.DispatcherTimer
            {
                Interval = TimeSpan.FromMilliseconds(120)
            };
            previewStatusTimer.Tick += PreviewStatusTimer_Tick;

            BlackedOutSlideBar.ValueChanged += PreviewMaskSetting_ValueChanged;
            FixedFrameSlideBar.ValueChanged += PreviewMaskSetting_ValueChanged;
            Add_Copyright.Checked += Add_Copyright_CheckedChanged;
            Add_Copyright.Unchecked += Add_Copyright_CheckedChanged;

            string dllDirectory = System.IO.Path.Combine(AppDomain.CurrentDomain.BaseDirectory);
            Environment.CurrentDirectory = dllDirectory;
            // 現在の実行ディレクトリを取得
            string currentDirectory = Environment.CurrentDirectory;

            // デバッグ情報としてコンソールに表示
            Console.WriteLine($"現在の実行ディレクトリ: {currentDirectory}");
            if (File.Exists("WoLNamesBlackedOut_DLL.dll"))
            {
                Console.WriteLine("DLLファイルが存在します");
            }
            else
            {
                Console.WriteLine("DLLファイルが存在しません");
            }
            StartupTrace("ctor:before gpu detect");

            char gpuvendor = SafeGetGpuVendor();
            StartupTrace($"ctor:after gpu detect {gpuvendor}");
            if (gpuvendor == 'N')   //NVIDIAの場合
            {
                codec = "hevc_nvenc";
                hwaccel = "cuda";
                preset = "slow"; // NVIDIAのプリセットを設定
                useblackedout = true;
                // ConvertButton.IsEnabled = false;  // TODO: ConvertButton not found in XAML
            }
            else if (gpuvendor == 'A')  //AMDの場合
            {
                codec = "hevc_amf";
                hwaccel = "d3d11va";
                preset = "quality"; // AMDのプリセットを設定（必要に応じて変更可能）
                // ConvertButton.IsEnabled = false;  // TODO: ConvertButton not found in XAML
                useblackedout = true;
            }
            else if (gpuvendor == 'I')　//Intelの場合
            {
                codec = "hevc_qsv";
                hwaccel = "qsv";
                preset = "slow"; // Intelのプリセットを設定（必要に応じて変更可能）
                // ConvertButton.IsEnabled = false;  // TODO: ConvertButton not found in XAML
                useblackedout = true;
            }
            else //その他のベンダーの場合(gpuvendor == 'X')
            {
                UIControl_enable_false();
                InfoBar.Message = "Sorry, we could not find any available hardware video encoders. The app cannot edit the video.";
                InfoBar.Severity = InfoBarSeverity.Error;
                InfoBar.IsOpen = true;
                useblackedout = false;
                InfoBar.Visibility = Visibility.Visible;
            }

            //if (gpuvendor != 'X')
            {
                PickAFileButton.IsEnabled = true;
            }
            SetPreviewButtonVisual(false);
            SetBlackedOutButtonVisual(false);
            StartupTrace("ctor:end");
        }

        private void TryResizeAppWindow(int logicalWidth, int logicalHeight)
        {
            if (isWindowClosing)
            {
                return;
            }

            try
            {
                double dpiScale = WindowHelper.GetWindowDpiScale(this);
                int safeLogicalWidth = Math.Max(320, logicalWidth);
                //int safeLogicalHeight = Math.Max(240, logicalHeight);
                int safeLogicalHeight = logicalHeight;
                int physicalWidth = Math.Max(1, (int)Math.Round(safeLogicalWidth * dpiScale));
                int physicalHeight = Math.Max(1, (int)Math.Round(safeLogicalHeight * dpiScale));

                var targetSize = new Windows.Graphics.SizeInt32
                {
                    Width = physicalWidth,
                    Height = physicalHeight
                };

                this.AppWindow.Resize(targetSize);
            }
            catch (ArgumentException ex)
            {
                Debug.WriteLine($"AppWindow.Resize ignored: {ex.Message}");
            }
            catch (Exception ex)
            {
                Debug.WriteLine($"AppWindow.Resize failed: {ex.Message}");
            }
        }

        private static string GetComboText(ComboBox comboBox, string fallback)
        {
            if (comboBox.SelectedItem is string s)
            {
                return s;
            }

            if (comboBox.SelectedItem is ComboBoxItem item && item.Content is string cs)
            {
                return cs;
            }

            if (comboBox.SelectedValue is string sv)
            {
                return sv;
            }

            return fallback;
        }

        private string GetEffectiveCodecForCurrentRun()
        {
            bool forXEnabled = ForX?.IsChecked == true;
            if (!forXEnabled)
            {
                return codec;
            }

            if (codec.Contains("nvenc", StringComparison.OrdinalIgnoreCase))
            {
                return "h264_nvenc";
            }

            if (codec.Contains("amf", StringComparison.OrdinalIgnoreCase))
            {
                return "h264_amf";
            }

            if (codec.Contains("qsv", StringComparison.OrdinalIgnoreCase))
            {
                return "h264_qsv";
            }

            return "h264_nvenc";
        }

        private float GetYoloThreshold()
        {
            double value = YoloThresholdSlider?.Value ?? 0.15;
            return (float)Math.Clamp(value, 0.01, 0.50);
        }

        private void SetPreviewButtonVisual(bool running)
        {
            if (isWindowClosing)
            {
                return;
            }

            try
            {
                PreviewButtonText.Text = running
                    ? GetLocalizedString("Runtime.PreviewStop", " Preview Stop")
                    : GetLocalizedString("Runtime.PreviewStart", " Preview Start");
                PreviewButtonIcon.Glyph = running ? "\uE71A" : "\uF19D";
            }
            catch (ArgumentException)
            {
                isWindowClosing = true;
            }
        }

        private void SetBlackedOutButtonVisual(bool running)
        {
            if (isWindowClosing)
            {
                return;
            }

            try
            {
                BlackedOutStartButtonText.Text = running
                    ? GetLocalizedString("Runtime.BlackedOutStop", " BlackedOut Stop")
                    : GetLocalizedString("Runtime.BlackedOutStart", " BlackedOut Start");
                BlackedOutStartButtonIcon.Glyph = running ? "\uE71A" : "\uF5B0";
            }
            catch (ArgumentException)
            {
                isWindowClosing = true;
            }
        }

        private void EnterCancelPendingState()
        {
            cancel_state = true;
            cancel_pending_state = true;

            if (running_state)
            {
                UIControl_enable_false();
                BlackedOutStartButton.IsEnabled = false;
                StopButton.IsEnabled = false;
            }
        }

        private static bool IsSliderMaskType(string value)
        {
            return value == "Mosaic" || value == "Blur" || value == "Inpaint";
        }

        private void ConfigureMaskSliderForType(Slider slider, string value)
        {
            if (value == "Inpaint")
            {
                slider.Minimum = 1;
                slider.Maximum = 200;
                if (slider.Value <= 5)
                {
                    slider.Value = 70;
                }
                return;
            }

            slider.Minimum = 1;
            slider.Maximum = 5;
            if (slider.Value > slider.Maximum)
            {
                slider.Value = slider.Maximum;
            }
        }

        private void MinimizeButton_Click(object sender, RoutedEventArgs e)
        {
            if (this.AppWindow.Presenter is OverlappedPresenter presenter)
                presenter.Minimize();
        }

        private void MaximizeButton_Click(object sender, RoutedEventArgs e)
        {
            if (this.AppWindow is AppWindow appWindow)
            {
                var presenter = appWindow.Presenter as OverlappedPresenter;
                if (presenter != null)
                {
                    if (presenter.State == OverlappedPresenterState.Maximized)
                    {
                        presenter.Restore();
                        MaximiseIcon.Glyph = "\uE922";
                    }
                    else
                    {
                        presenter.Maximize();
                        MaximiseIcon.Glyph = "\uE923";
                    }
                }
            }
        }

        private void CloseButton_Click(object sender, RoutedEventArgs e)
        {
            if (isWindowClosing)
            {
                return;
            }

            BeginWindowClosing();
            try
            {
                Application.Current.Exit();
            }
            catch (Exception ex)
            {
                Debug.WriteLine($"Application.Current.Exit failed: {ex.Message}");
                try
                {
                    this.Close();
                }
                catch (Exception ex2)
                {
                    Debug.WriteLine($"Window.Close failed: {ex2.Message}");
                    try
                    {
                        Environment.Exit(0);
                    }
                    catch
                    {
                    }
                }
            }
        }

        private void AppWindow_Closing(AppWindow sender, AppWindowClosingEventArgs args)
        {
            BeginWindowClosing();
        }

        private void Window_Closed(object sender, WindowEventArgs e)
        {
            BeginWindowClosing();

            //ウィンドウが閉じた際のイベント
            var localSettings = TryGetLocalSettings();
            if (localSettings != null)
            {
                // チェックボックスの状態を保存する
                try { localSettings.Values["Add_Copyright"] = Add_Copyright?.IsChecked == true; } catch { }
                try { localSettings.Values["Bitrate"] = BitrateSlideBar?.Value ?? 0.0; } catch { }
                try { localSettings.Values["AppTheme"] = RootGrid.ActualTheme == ElementTheme.Light ? "Light" : "Dark"; } catch { }
                try { localSettings.Values["OptimizationProfile"] = 3; } catch { }
                try { localSettings.Values["DeviceName"] = ""; } catch { }
                try { localSettings.Values["DebugLogExport"] = DebugLogExport?.IsChecked == true; } catch { }
                try { localSettings.Values["YoloThresholdSlider"] = YoloThresholdSlider?.Value ?? 0.0; } catch { }
                try { localSettings.Values["ExcludeByNameEnabled"] = excludeByNameEnabled; } catch { }
                try { localSettings.Values["OcrExpandPixels"] = ocrExpandPixels; } catch { }
                try { localSettings.Values["OcrMaxRoisPerFrame"] = ocrMaxRoisPerFrame; } catch { }
                try { localSettings.Values["TextSimilarityPercent"] = textSimilarityPercent; } catch { }
                try { localSettings.Values["MaskExcludeTextCsv"] = maskExcludeTextCsv ?? string.Empty; } catch { }
                try { localSettings.Values[UiLanguagePreferenceKey] = LanguageJP?.IsChecked == true; } catch { }
            }

            // tempDirectory 内の "tmp_wol_*.*" にマッチするファイル一覧を取得する
            string tempDirectory = System.IO.Path.GetTempPath();
            string[] tmpFiles = Directory.GetFiles(tempDirectory, "tmp_wol_*.*");
            foreach (string file in tmpFiles)
            {
                try
                {
                    File.Delete(file);
                }
                catch (Exception ex)
                {
                    // 削除に失敗した場合の例外処理（ログ出力などを検討してください）
                    Console.WriteLine($"ファイル '{file}' の削除に失敗しました: {ex.Message}");
                }
            }
        }
        private void ToggleTheme()
        {
            // ルートの Grid（ここでは RootGrid）の ActualTheme プロパティで、現在のテーマをチェック
            if (RootGrid.ActualTheme == ElementTheme.Light)
            {
                ApplyTheme(ElementTheme.Dark);
            }
            else
            {
                ApplyTheme(ElementTheme.Light);
            }
        }

        private void ApplyTheme(ElementTheme theme)
        {
            RootGrid.RequestedTheme = theme;
            if (this.Content is FrameworkElement root)
            {
                root.RequestedTheme = theme;
            }
        }

        private void TryEnableMicaBackdrop()
        {
            try
            {
                windowMicaBackdrop.Kind = MicaKind.BaseAlt;
                this.SystemBackdrop = windowMicaBackdrop;
            }
            catch (Exception ex)
            {
                Debug.WriteLine($"Mica backdrop initialization failed: {ex.Message}");
            }
        }

        private void ToggleThemeButton_Click(object sender, RoutedEventArgs e)
        {
            ToggleTheme();
        }

        private void Timer_Tick(object? sender, object e)
        {
            if (isWindowClosing)
            {
                return;
            }

            // 経過時間を秒単位で表示
            double elapsedSeconds = stopwatch.Elapsed.TotalSeconds;
            Elapsed.Text = elapsedSeconds.ToString("F1"); // 小数点以下1桁まで表示
            var latestStatus = FrameProcessor.LatestStatusMessage;
            if (!string.IsNullOrWhiteSpace(latestStatus))
            {
                FFMpeg_text.Text = latestStatus;
            }
            int frame_count = SafeGetTotalFrameCount();
            if (frame_count > 0)
            {
                double fps = FrameProcessor.GetRecentFps(10.0);
                if (fps <= 0)
                {
                    fps = elapsedSeconds > 0 ? ((double)frame_count / elapsedSeconds) : 0.0;
                }
                double duration = (v_end_time - v_start_time);
                double denom = Math.Max(1.0, v_fps * duration);
                double percentage = (double)frame_count / denom;
                percentage = Math.Clamp(percentage, 0.0, 1.0);
                double eta = percentage > 0.0001
                    ? (elapsedSeconds * (1 - percentage) / percentage + 0.5)
                    : 0.0;
                if (!double.IsFinite(eta) || eta < 0)
                {
                    eta = 0.0;
                }
                FPS.Text = fps.ToString("F2");
                ETA.Text = eta.ToString("F2");
                ProgressBar.Value = percentage * 100;
            }
        }
        private async void RootGrid_Drop(object sender, DragEventArgs e)
        {
            if (e.DataView.Contains(StandardDataFormats.StorageItems))
            {
                var items = await e.DataView.GetStorageItemsAsync();
                if (items.Count > 0 && items[0] is StorageFile file)
                {
                    var ext = System.IO.Path.GetExtension(file.Name).ToLower();
                    var allowed = new[] { ".jpg", ".jpeg", ".png", ".mp4" };
                    if (!allowed.Contains(ext))
                    {
                        var dialog = new ContentDialog
                        {
                            Title = "Unsupported files",
                            Content = "Only .jpg, .jpeg, .png, and .mp4 are supported",
                            CloseButtonText = "OK",
                            XamlRoot = this.Content.XamlRoot
                        };
                        await dialog.ShowAsync();
                        return;
                    }
                    await HandleFileSelectedAsync(file);
                }

            }
        }
        private void RootGrid_DragOver(object sender, DragEventArgs e)
        {
            if (e.DataView.Contains(StandardDataFormats.StorageItems))
                e.AcceptedOperation = DataPackageOperation.Copy;
            else
                e.AcceptedOperation = DataPackageOperation.None;
            e.Handled = true;
        }
        private async Task HandleFileSelectedAsync(StorageFile file)
        {
            UIControl_enable_false();
            running_state = true;
            StopRealtimePreviewSession();
            suppressPreviewToggleEvent = true;
            PreviewButton.IsChecked = false;
            suppressPreviewToggleEvent = false;
            SetPreviewButtonVisual(false);

            // 新しいファイル読込時はcopyright操作状態を初期化
            copyrightDragging = false;
            copyrightOffsetX = 0;
            copyrightOffsetY = 0;
            copyrightZoomScale = 1.0;
            copyrightImagePath = null;
            ClearPreviewFrameCache();

            PickAFileOutputTextBlock.Text = "";

            if (file == null)
            {
                PickAFileOutputTextBlock.Text = "Operation cancelled.";
                SafeSetProgressIndeterminate(false);
                UIControl_enable_true();
                running_state = false;
                return;
            }

            var fileExtension = file.FileType.ToLower();
            bool isImageFile = fileExtension == ".jpg" || fileExtension == ".jpeg" || fileExtension == ".png";

            SafeSetProgressIndeterminate(true);
            if (isImageFile)
            {
                await WaitForProgressIndicatorAsync();
            }

            PickAFileOutputTextBlock.Text = file.Name;
            v_file_path = file.Path;

            if (isImageFile)
            {
                string tempDirectory = System.IO.Path.GetTempPath();
                string fileName = $"tmp_wol_{DateTime.Now:yyyyMMddHHmmssfff}.png";
                string outputPath = System.IO.Path.Combine(tempDirectory, fileName);

                File.Copy(v_file_path, outputPath, true);

                RectInfo[] rectInfos = savedRects.Select(rect => new RectInfo
                {
                    x = (int)rect.X,
                    y = (int)rect.Y,
                    width = (int)rect.Width,
                    height = (int)rect.Height
                }).ToArray();

                SolidColorBrush BlackedOut_color_icon_brush = (SolidColorBrush)BlackedOut_color_icon.Foreground;
                Color BlackedOut_color_icon_color = BlackedOut_color_icon_brush.Color;
                ColorInfo BlackedOut_color_icon_color_info = new ColorInfo
                {
                    r = BlackedOut_color_icon_color.R,
                    g = BlackedOut_color_icon_color.G,
                    b = BlackedOut_color_icon_color.B
                };

                SolidColorBrush FixedFrame_color_icon_brush = (SolidColorBrush)FixedFrame_color_icon.Foreground;
                Color FixedFrame_color_icon_color = FixedFrame_color_icon_brush.Color;
                ColorInfo FixedFrame_color_icon_color_Info = new ColorInfo
                {
                    r = FixedFrame_color_icon_color.R,
                    g = FixedFrame_color_icon_color.G,
                    b = FixedFrame_color_icon_color.B
                };

                await FrameProcessor.Runpreview_apiAsync(outputPath, rectInfos, rectInfos.Length, BlackedOut_color_icon_color_info, FixedFrame_color_icon_color_Info, Add_Copyright.IsChecked.Value, GetComboText(BlackedOut_ComboBox, "Solid"), GetComboText(FixedFrame_ComboBox, "Solid"), (int)BlackedOutSlideBar.Value, (int)FixedFrameSlideBar.Value, GetYoloThreshold(), copyrightImagePath ?? string.Empty);

                last_preview_image = outputPath;
                using (var stream = await file.OpenStreamForReadAsync())
                {
                    var bitmapImage = new BitmapImage(new Uri(outputPath));
                    bitmapImage.ImageOpened += (s, ev) =>
                    {
                        if (isWindowClosing)
                        {
                            return;
                        }

                        var originalHeight = bitmapImage.PixelHeight;
                        scaleFactor = image_preview.Height / originalHeight;

                        // 論理サイズをDPIスケールで変換
                        int logicalWidth = 800 + (int)(bitmapImage.PixelWidth * scaleFactor) - 280;
                        int logicalHeight = 850;
                        TryResizeAppWindow(logicalWidth, logicalHeight);


                        image_preview.Source = bitmapImage;
                        SafeSetProgressIndeterminate(false);
                        UIControl_enable_true();
                        running_state = false;
                        RemoveAllRectangles();
                    };
                    image_preview.Source = bitmapImage;
                    SaveImageButton.IsEnabled = true;
                }
            }
            else if (fileExtension == ".mp4")
            {
                last_preview_image = string.Empty;
                var properties = await GetVideoProperties(file);
                if (properties != null)
                {
                    if (int.TryParse(properties["width"], out int width))
                        v_width = width;
                    if (int.TryParse(properties["height"], out int height))
                        v_height = height;
                    double frameRate = 0;
                    if (properties.TryGetValue("avg_frame_rate", out string avgFrameRateText)
                        && double.TryParse(avgFrameRateText, NumberStyles.Float, CultureInfo.InvariantCulture, out double avgFrameRate)
                        && avgFrameRate > 0)
                    {
                        frameRate = avgFrameRate;
                    }
                    else if (properties.TryGetValue("r_frame_rate", out string rawFrameRateText)
                        && double.TryParse(rawFrameRateText, NumberStyles.Float, CultureInfo.InvariantCulture, out double rawFrameRate)
                        && rawFrameRate > 0)
                    {
                        frameRate = rawFrameRate;
                    }

                    v_fps = Math.Max(1, (int)Math.Round(frameRate > 0 ? frameRate : 30.0, MidpointRounding.AwayFromZero));

                    double durationSeconds = 0;
                    if (properties.TryGetValue("duration", out string durationText))
                    {
                        _ = double.TryParse(durationText, NumberStyles.Float, CultureInfo.InvariantCulture, out durationSeconds);
                    }

                    if (int.TryParse(properties.GetValueOrDefault("nb_frames", "0"), NumberStyles.Integer, CultureInfo.InvariantCulture, out int nbFrames) && nbFrames > 0)
                    {
                        v_nb_frames = nbFrames;
                    }
                    else if (durationSeconds > 0)
                    {
                        v_nb_frames = Math.Max(1, (int)Math.Round(durationSeconds * Math.Max(frameRate, 1.0), MidpointRounding.AwayFromZero));
                    }
                    else
                    {
                        v_nb_frames = Math.Max(1, v_nb_frames);
                    }

                    int totalSeconds = 0;
                    if (durationSeconds > 0)
                    {
                        totalSeconds = Math.Max(1, (int)Math.Floor(durationSeconds));
                    }
                    else
                    {
                        totalSeconds = Math.Max(1, (int)Math.Floor(v_nb_frames / (double)Math.Max(1, v_fps)));
                    }

                    int totalMinutes = totalSeconds / 60;
                    int remainingSeconds = totalSeconds % 60;

                    Start_min.Value = 0;
                    Start_sec.Value = 0;
                    End_min.Value = totalMinutes;
                    End_sec.Value = remainingSeconds;
                    Start_min.Maximum = totalMinutes;
                    End_min.Maximum = totalMinutes;
                    FrameSlideBar.Value = 0;
                    FrameSlideBar.Maximum = totalSeconds;
                    FrameTextBlock_e.Text = $"{totalMinutes}:{remainingSeconds:D2}";

                    v_color_primaries = properties["color_primaries"];
                    v_hasAudio = properties.TryGetValue("has_audio", out var audioValue) && audioValue == "true";
                }

                BeginPreviewStatusFeedback(GetLocalizedString("Runtime.PreviewInitializing", "Preparing preview..."));
                bool opened = false;
                try
                {
                    opened = await StartRealtimePreviewSessionAsync(v_file_path);
                }
                finally
                {
                    EndPreviewStatusFeedback(opened
                        ? GetLocalizedString("Runtime.PreviewReady", "Preview ready")
                        : GetLocalizedString("Runtime.PreviewInitFailed", "Preview initialization failed"));
                }
                if (opened)
                {
                    previewFrameIndex = 0;
                    await PreviewSingleFrameAsync(0);
                    previewFrameIndex = 0;
                    suppressFrameSliderValueChanged = true;
                    FrameSlideBar.Value = FrameSlideBar.Minimum;
                    suppressFrameSliderValueChanged = false;
                }
            }

            SafeSetProgressIndeterminate(false);
            UIControl_enable_true();
            running_state = false;
        }

        private async Task PreviewSingleFrameAsync(int frameIndex, bool showBusyIndicator = false)
        {
            if (isWindowClosing || !previewSessionOpen || previewTickBusy)
            {
                return;
            }

            if (showBusyIndicator)
            {
                SetPreviewBusyIndicator(true);
            }

            previewTickBusy = true;
            try
            {
                var rectInfos = BuildRectInfos();
                var (nameColor, fixedColor) = BuildMaskColors();
                string blackedOut = GetComboText(BlackedOut_ComboBox, "Solid");
                string fixedFrame = GetComboText(FixedFrame_ComboBox, "Solid");
                int blackedOutParam = (int)BlackedOutSlideBar.Value;
                int fixedFrameParam = (int)FixedFrameSlideBar.Value;
                bool addCopyright = Add_Copyright.IsChecked.Value;

                int updateResult = await Task.Run(() => FrameProcessor.PreviewUpdateMask(
                    rectInfos,
                    rectInfos.Length,
                    nameColor,
                    fixedColor,
                    addCopyright,
                    blackedOut,
                    fixedFrame,
                    blackedOutParam,
                    fixedFrameParam,
                    copyrightOffsetX,
                    copyrightOffsetY,
                    (float)copyrightZoomScale,
                    excludeByNameEnabled,
                    ocrExpandPixels,
                    ocrMaxRoisPerFrame,
                    GetTextSimilarityThreshold(),
                    maskExcludeTextCsv));

                if (isWindowClosing || !previewSessionOpen)
                {
                    return;
                }

                if (updateResult != 0)
                {
                    return;
                }

                int expectedWidth = Math.Max(1, v_width);
                int expectedHeight = Math.Max(1, v_height);
                byte[] bgra = new byte[expectedWidth * expectedHeight * 4];

                var frameResult = await Task.Run(() =>
                {
                    int result = FrameProcessor.PreviewGetFrameBuffer(frameIndex, bgra, out int outWidth, out int outHeight);
                    return (result, outWidth, outHeight);
                });

                if (isWindowClosing || !previewSessionOpen)
                {
                    return;
                }

                if (frameResult.result != 0 || frameResult.outWidth <= 0 || frameResult.outHeight <= 0)
                {
                    return;
                }

                int required = frameResult.outWidth * frameResult.outHeight * 4;
                if (bgra.Length < required)
                {
                    bgra = new byte[required];
                    int retry = FrameProcessor.PreviewGetFrameBuffer(frameIndex, bgra, out int outWidth, out int outHeight);
                    if (retry != 0 || outWidth <= 0 || outHeight <= 0)
                    {
                        return;
                    }
                    frameResult = (retry, outWidth, outHeight);
                }

                lock (previewFrameCacheLock)
                {
                    lastPreviewFrameBgra = new byte[required];
                    Buffer.BlockCopy(bgra, 0, lastPreviewFrameBgra, 0, required);
                    lastPreviewFrameWidth = frameResult.outWidth;
                    lastPreviewFrameHeight = frameResult.outHeight;
                }

                if (previewBitmap == null || previewBitmap.PixelWidth != frameResult.outWidth || previewBitmap.PixelHeight != frameResult.outHeight)
                {
                    previewBitmap = new WriteableBitmap(frameResult.outWidth, frameResult.outHeight);
                    image_preview.Source = previewBitmap;
                    previewLayoutInitialized = false;
                }

                if (!previewLayoutInitialized)
                {
                    double previewHeight = image_preview.ActualHeight > 0 ? image_preview.ActualHeight : image_preview.Height;
                    if (previewHeight <= 0)
                    {
                        previewHeight = 800;
                    }

                    scaleFactor = previewHeight / frameResult.outHeight;

                    int logicalWidth = 800 + (int)(frameResult.outWidth * scaleFactor) - 280;
                    int logicalHeight = 850;
                    TryResizeAppWindow(logicalWidth, logicalHeight);
                    previewLayoutInitialized = true;
                }

                using (var pixelStream = previewBitmap.PixelBuffer.AsStream())
                {
                    pixelStream.Position = 0;
                    pixelStream.Write(bgra, 0, frameResult.outWidth * frameResult.outHeight * 4);
                }
                previewBitmap.Invalidate();
            }
            finally
            {
                previewTickBusy = false;
                if (showBusyIndicator)
                {
                    SetPreviewBusyIndicator(false);
                }
            }
        }
        private async void PickAFileButton_Click(object sender, Microsoft.UI.Xaml.RoutedEventArgs e)
        {
            var openPicker = new Windows.Storage.Pickers.FileOpenPicker();
            var window = App.MainWindow;
            var hWnd = WinRT.Interop.WindowNative.GetWindowHandle(window);
            WinRT.Interop.InitializeWithWindow.Initialize(openPicker, hWnd);

            openPicker.ViewMode = PickerViewMode.Thumbnail;
            openPicker.SuggestedStartLocation = PickerLocationId.VideosLibrary;
            openPicker.FileTypeFilter.Add(".jpg");
            openPicker.FileTypeFilter.Add(".jpeg");
            openPicker.FileTypeFilter.Add(".png");
            openPicker.FileTypeFilter.Add(".mp4");

            var file = await openPicker.PickSingleFileAsync();
            await HandleFileSelectedAsync(file);

        }

        private async void SelectCopyrightImageButton_Click(object sender, RoutedEventArgs e)
        {
            var picker = new FileOpenPicker();
            InitializeWithWindow.Initialize(picker, WindowNative.GetWindowHandle(this));
            picker.ViewMode = PickerViewMode.Thumbnail;
            picker.SuggestedStartLocation = PickerLocationId.PicturesLibrary;
            picker.FileTypeFilter.Add(".png");
            picker.FileTypeFilter.Add(".jpg");
            picker.FileTypeFilter.Add(".jpeg");

            var file = await picker.PickSingleFileAsync();
            if (file == null)
            {
                return;
            }

            copyrightImagePath = file.Path;

            if (previewSessionOpen)
            {
                FrameProcessor.SetCopyrightImagePath(copyrightImagePath);
                if (previewTimer.IsEnabled)
                {
                    previewTimer.Stop();
                    _ = RefreshCurrentPreviewFrameIfPausedAsync();
                    previewTimer.Start();
                }
                else
                {
                    _ = RefreshCurrentPreviewFrameIfPausedAsync();
                }
            }
        }

        public class FrameProcessor
        {
            [UnmanagedFunctionPointer(CallingConvention.StdCall)]
            private delegate void ProgressCallback(int processed_frames, int total_frames, double elapsed_seconds);

            [UnmanagedFunctionPointer(CallingConvention.StdCall)]
            private delegate void StatusCallback(IntPtr message);

            [StructLayout(LayoutKind.Sequential, CharSet = CharSet.Unicode)]
            private struct NativePreviewParams
            {
                [MarshalAs(UnmanagedType.LPWStr)]
                public string file_path;
                [MarshalAs(UnmanagedType.LPWStr)]
                public string model_path;
                [MarshalAs(UnmanagedType.LPWStr)]
                public string copyright_image_path;
                public int input_width;
                public int input_height;
                public float conf_threshold;
                public float iou_threshold;
            }

            [StructLayout(LayoutKind.Sequential, CharSet = CharSet.Unicode)]
            private struct NativePreviewMaskParams
            {
                public int blacked_type;
                public ColorInfo name_color;
                public int blackedout_param;
                public int fixmask_type;
                public ColorInfo fixframe_color;
                public int fixedFrame_param;
                public int fixed_rect_count;
                [MarshalAs(UnmanagedType.ByValArray, SizeConst = 64)]
                public RectInfo[] fixed_rects;
                [MarshalAs(UnmanagedType.I1)]
                public bool enable_copyright;
                public int copyright_offset_x;
                public int copyright_offset_y;
                public float copyright_scale;
                [MarshalAs(UnmanagedType.I1)]
                public bool exclude_by_name_enabled;
                public int ocr_expand_pixels;
                public int ocr_max_rois_per_frame;
                public float text_similarity_threshold;
                [MarshalAs(UnmanagedType.ByValTStr, SizeConst = 512)]
                public string mask_exclude_text_csv;
                [MarshalAs(UnmanagedType.ByValArray, SizeConst = 4)]
                public int[] reserved;
            }

            [DllImport("WoLNamesBlackedOut_DLL.dll", CallingConvention = CallingConvention.StdCall)]
            private static extern int ProcessVideo(
                [MarshalAs(UnmanagedType.LPUTF8Str)] string input_video_path,
                [MarshalAs(UnmanagedType.LPUTF8Str)] string output_video_path,
                [MarshalAs(UnmanagedType.LPUTF8Str)] string codec,
                [MarshalAs(UnmanagedType.LPUTF8Str)] string hwaccel,
                int width, int height, int fps,
                double trim_start_seconds,
                double trim_end_seconds,
                float conf_threshold,
                [MarshalAs(UnmanagedType.LPUTF8Str)] string color_primaries,
                [In] RectInfo[] rects, int count,
                ColorInfo name_color, ColorInfo fixframe_color,
                [MarshalAs(UnmanagedType.I1)] bool copyright,
                int blackedOut,
                int fixedFrame,
                int blackedout_param,
                int fixedFrame_param,
                int copyright_offset_x,
                int copyright_offset_y,
                float copyright_scale,
                [MarshalAs(UnmanagedType.LPUTF8Str)] string copyright_image_path,
                [MarshalAs(UnmanagedType.I1)] bool exclude_by_name_enabled,
                int ocr_expand_pixels,
                int ocr_max_rois_per_frame,
                float text_similarity_threshold,
                [MarshalAs(UnmanagedType.LPUTF8Str)] string mask_exclude_text_csv,
                [MarshalAs(UnmanagedType.LPUTF8Str)] string bitrate,
                [MarshalAs(UnmanagedType.LPUTF8Str)] string preset,
                [MarshalAs(UnmanagedType.I1)] bool disable_audio,
                ProgressCallback progress_callback
            );

            [DllImport("WoLNamesBlackedOut_DLL.dll", CallingConvention = CallingConvention.StdCall)]
            [return: MarshalAs(UnmanagedType.I1)]
            private static extern bool CancelProcess();

            [DllImport("WoLNamesBlackedOut_DLL.dll", CallingConvention = CallingConvention.StdCall)]
            private static extern void SetStatusCallback(StatusCallback callback);

            [DllImport("WoLNamesBlackedOut_DLL.dll", CallingConvention = CallingConvention.StdCall)]
            private static extern void SetWinMLStatusCallback(StatusCallback callback);

            [DllImport("WoLNamesBlackedOut_DLL.dll", CallingConvention = CallingConvention.StdCall)]
            private static extern int PreviewOpen(ref NativePreviewParams args);

            [DllImport("WoLNamesBlackedOut_DLL.dll", CallingConvention = CallingConvention.StdCall)]
            private static extern int PreviewSetCopyrightImagePath([MarshalAs(UnmanagedType.LPWStr)] string image_path);

            [DllImport("WoLNamesBlackedOut_DLL.dll", CallingConvention = CallingConvention.StdCall)]
            private static extern int PreviewUpdateParams(ref NativePreviewMaskParams args);

            [DllImport("WoLNamesBlackedOut_DLL.dll", CallingConvention = CallingConvention.StdCall)]
            private static extern int PreviewSetCopyrightOffset(int offset_x, int offset_y);

            [DllImport("WoLNamesBlackedOut_DLL.dll", CallingConvention = CallingConvention.StdCall)]
            private static extern int PreviewGetFrame(
                int frame_index,
                [Out] byte[] out_rgba_buffer,
                int buffer_size,
                out int out_width,
                out int out_height);

            [DllImport("WoLNamesBlackedOut_DLL.dll", CallingConvention = CallingConvention.StdCall)]
            private static extern int PreviewClose();

            private static readonly ProgressCallback ProgressCallbackInstance = OnProgress;
            private static readonly StatusCallback StatusCallbackInstance = OnStatus;
            private static int _latestProcessedFrames;
            private static readonly object StatusLock = new object();
            private static readonly object ProgressLock = new object();
            private static readonly Queue<(long timestamp, int processedFrames)> ProgressSamples = new();
            private static string _latestStatusMessage = string.Empty;
            private static bool _statusCallbackRegistered;

            public static int LatestProcessedFrames => Volatile.Read(ref _latestProcessedFrames);

            public static string LatestStatusMessage
            {
                get
                {
                    lock (StatusLock)
                    {
                        return _latestStatusMessage;
                    }
                }
            }

            public static void SetCopyrightImagePath(string imagePath)
            {
                try
                {
                    PreviewSetCopyrightImagePath(imagePath ?? string.Empty);
                }
                catch (Exception ex)
                {
                    Debug.WriteLine($"SetCopyrightImagePath failed: {ex.Message}");
                }
            }

            private static void OnProgress(int processed_frames, int total_frames, double elapsed_seconds)
            {
                Interlocked.Exchange(ref _latestProcessedFrames, processed_frames);
                lock (ProgressLock)
                {
                    long now = Stopwatch.GetTimestamp();
                    ProgressSamples.Enqueue((now, processed_frames));
                    PruneProgressSamples(now, 10.0);
                }
            }

            private static void PruneProgressSamples(long now, double windowSeconds)
            {
                long cutoff = now - (long)(windowSeconds * Stopwatch.Frequency);
                while (ProgressSamples.Count > 0 && ProgressSamples.Peek().timestamp < cutoff)
                {
                    ProgressSamples.Dequeue();
                }
            }

            public static double GetRecentFps(double windowSeconds = 10.0)
            {
                lock (ProgressLock)
                {
                    if (ProgressSamples.Count < 2)
                    {
                        return 0.0;
                    }

                    long now = Stopwatch.GetTimestamp();
                    PruneProgressSamples(now, windowSeconds);
                    if (ProgressSamples.Count < 2)
                    {
                        return 0.0;
                    }

                    var first = ProgressSamples.Peek();
                    var last = ProgressSamples.Last();
                    double elapsed = (last.timestamp - first.timestamp) / (double)Stopwatch.Frequency;
                    if (elapsed <= 0)
                    {
                        return 0.0;
                    }

                    int frames = last.processedFrames - first.processedFrames;
                    return frames > 0 ? frames / elapsed : 0.0;
                }
            }

            private static void OnStatus(IntPtr message)
            {
                string text;
                try
                {
                    text = message == IntPtr.Zero ? string.Empty : Marshal.PtrToStringUTF8(message) ?? string.Empty;
                }
                catch (Exception ex)
                {
                    Debug.WriteLine($"Status callback decode failed: {ex.Message}");
                    text = string.Empty;
                }

                lock (StatusLock)
                {
                    _latestStatusMessage = text;
                }
            }

            private static void TryRegisterStatusCallback()
            {
                if (_statusCallbackRegistered)
                {
                    return;
                }

                bool registered = false;
                try
                {
                    SetStatusCallback(StatusCallbackInstance);
                    registered = true;
                }
                catch (Exception ex) when (ex is DllNotFoundException || ex is EntryPointNotFoundException)
                {
                    Debug.WriteLine($"SetStatusCallback unavailable: {ex.Message}");
                }

                try
                {
                    SetWinMLStatusCallback(StatusCallbackInstance);
                    registered = true;
                }
                catch (Exception ex) when (ex is DllNotFoundException || ex is EntryPointNotFoundException)
                {
                    Debug.WriteLine($"SetWinMLStatusCallback unavailable: {ex.Message}");
                }

                _statusCallbackRegistered = registered;
            }

            public static void ResetLatestStatusMessage()
            {
                lock (StatusLock)
                {
                    _latestStatusMessage = string.Empty;
                }
            }

            public static void PrepareStatusTracking()
            {
                TryRegisterStatusCallback();
                ResetLatestStatusMessage();
            }

            public static int MaskTypeToNative(MaskTypeKind value)
            {
                return (int)value;
            }

            public static MaskTypeKind GetMaskTypeKind(string? value)
            {
                return value switch
                {
                    "Inpaint" => MaskTypeKind.Inpaint,
                    "Mosaic" => MaskTypeKind.Mosaic,
                    "Blur" => MaskTypeKind.Blur,
                    "No_Inference" => MaskTypeKind.NoInference,
                    _ => MaskTypeKind.Solid,
                };
            }

            private static string ResolveModelPath()
            {
                return System.IO.Path.Combine(AppContext.BaseDirectory, "my_yolov8m_s.onnx");
            }

            private static async Task<(int width, int height)> GetImageDimensionsAsync(string filePath)
            {
                var file = await StorageFile.GetFileFromPathAsync(filePath);
                using var stream = await file.OpenReadAsync();
                var decoder = await BitmapDecoder.CreateAsync(stream);
                return ((int)decoder.PixelWidth, (int)decoder.PixelHeight);
            }

            private static async Task SavePreviewBufferToPngAsync(string filePath, byte[] bgra, int width, int height)
            {
                var file = await StorageFile.GetFileFromPathAsync(filePath);
                using var stream = await file.OpenAsync(FileAccessMode.ReadWrite);
                stream.Size = 0;

                var encoder = await BitmapEncoder.CreateAsync(BitmapEncoder.PngEncoderId, stream);
                encoder.SetPixelData(BitmapPixelFormat.Bgra8, BitmapAlphaMode.Ignore,
                    (uint)width, (uint)height, 96, 96, bgra);
                await encoder.FlushAsync();
            }

            public static bool TryCancelProcess()
            {
                try
                {
                    return CancelProcess();
                }
                catch (Exception ex) when (ex is DllNotFoundException || ex is EntryPointNotFoundException)
                {
                    Debug.WriteLine($"CancelProcess unavailable: {ex.Message}");
                    return false;
                }
            }

            public static Task<int> RunDmlMainAsync(
                string inputVideoPath, string outputVideoPath, string codec, string hwaccel,
                int width, int height, int fps, double trimStartSeconds, double trimEndSeconds, float confThreshold, string colorPrimaries, RectInfo[] rects,
                int count, ColorInfo nameColor, ColorInfo fixframeColor,
                bool copyright, int blackedOut, int fixedFrame, int blackedout_param, int fixedFrame_param,
                int copyrightOffsetX, int copyrightOffsetY, float copyrightScale, string copyrightImagePath,
                string bitrate, string preset, bool disableAudio,
                bool excludeByNameEnabled = false,
                int ocrExpandPixels = 2,
                int ocrMaxRoisPerFrame = 6,
                float textSimilarityThreshold = 0.85f,
                string maskExcludeTextCsv = "")
            {
                return Task.Run(() =>
                {
                    PrepareStatusTracking();
                    Interlocked.Exchange(ref _latestProcessedFrames, 0);
                    return ProcessVideo(inputVideoPath, outputVideoPath, codec, hwaccel, width, height, fps, trimStartSeconds, trimEndSeconds, confThreshold,
                        colorPrimaries, rects, count, nameColor, fixframeColor, copyright,
                        blackedOut, fixedFrame, blackedout_param, fixedFrame_param,
                        copyrightOffsetX, copyrightOffsetY, copyrightScale, copyrightImagePath ?? string.Empty,
                        excludeByNameEnabled,
                        ocrExpandPixels,
                        ocrMaxRoisPerFrame,
                        textSimilarityThreshold,
                        maskExcludeTextCsv ?? string.Empty,
                        bitrate, preset, disableAudio,
                        ProgressCallbackInstance);
                });
            }

            public static Task<int> RunTrtMainAsync(
                string inputVideoPath, string outputVideoPath, string codec, string hwaccel,
                int width, int height, int fps, double trimStartSeconds, double trimEndSeconds, float confThreshold, string colorPrimaries, RectInfo[] rects,
                int count, ColorInfo nameColor, ColorInfo fixframeColor,
                 bool copyright, int blackedOut, int fixedFrame, int blackedout_param, int fixedFrame_param,
                 int copyrightOffsetX, int copyrightOffsetY, float copyrightScale, string copyrightImagePath,
                 string bitrate, string preset, bool disableAudio,
                 bool excludeByNameEnabled = false,
                 int ocrExpandPixels = 2,
                 int ocrMaxRoisPerFrame = 6,
                 float textSimilarityThreshold = 0.85f,
                 string maskExcludeTextCsv = "")
            {
                return RunDmlMainAsync(inputVideoPath, outputVideoPath, codec, hwaccel, width, height, fps, trimStartSeconds, trimEndSeconds, confThreshold,
                    colorPrimaries, rects, count, nameColor, fixframeColor, copyright,
                    blackedOut, fixedFrame, blackedout_param, fixedFrame_param,
                    copyrightOffsetX, copyrightOffsetY, copyrightScale, copyrightImagePath,
                    bitrate, preset, disableAudio,
                    excludeByNameEnabled,
                    ocrExpandPixels,
                    ocrMaxRoisPerFrame,
                    textSimilarityThreshold,
                    maskExcludeTextCsv);
            }
            public static async Task<int> Runpreview_apiAsync(
                string image_path_str,
                RectInfo[] rects,
                int count, ColorInfo nameColor, ColorInfo fixframeColor,
                bool copyright, string blackedOut, string fixedFrame, int blackedout_param, int fixedFrame_param, float confThreshold,
                string copyrightImagePath)
            {
                PrepareStatusTracking();

                var previewParams = new NativePreviewParams
                {
                    file_path = image_path_str,
                    model_path = ResolveModelPath(),
                    copyright_image_path = copyrightImagePath ?? string.Empty,
                    input_width = 640,
                    input_height = 640,
                    conf_threshold = confThreshold,
                    iou_threshold = 0.45f,
                };

                var openResult = PreviewOpen(ref previewParams);
                if (openResult != 0)
                {
                    return openResult;
                }

                try
                {
                    var nativeRects = new RectInfo[64];
                    var rectCount = Math.Min(count, nativeRects.Length);
                    Array.Copy(rects, nativeRects, rectCount);

                    var maskParams = new NativePreviewMaskParams
                    {
                        blacked_type = MaskTypeToNative(GetMaskTypeKind(blackedOut)),
                        name_color = nameColor,
                        blackedout_param = blackedout_param,
                        fixmask_type = MaskTypeToNative(GetMaskTypeKind(fixedFrame)),
                        fixframe_color = fixframeColor,
                        fixedFrame_param = fixedFrame_param,
                        fixed_rect_count = rectCount,
                        fixed_rects = nativeRects,
                        enable_copyright = copyright,
                        copyright_offset_x = 0,
                        copyright_offset_y = 0,
                        copyright_scale = 1.0f,
                        exclude_by_name_enabled = false,
                        ocr_expand_pixels = 2,
                        ocr_max_rois_per_frame = 6,
                        text_similarity_threshold = 0.85f,
                        mask_exclude_text_csv = string.Empty,
                        reserved = new int[4],
                    };

                    var updateResult = PreviewUpdateParams(ref maskParams);
                    if (updateResult != 0)
                    {
                        return updateResult;
                    }

                    var (width, height) = await GetImageDimensionsAsync(image_path_str);
                    if (width <= 0 || height <= 0)
                    {
                        return -10;
                    }

                    var rgba = new byte[width * height * 4];
                    var frameResult = PreviewGetFrame(0, rgba, rgba.Length, out int outWidth, out int outHeight);
                    if (frameResult != 0)
                    {
                        return frameResult;
                    }

                    if (outWidth <= 0 || outHeight <= 0)
                    {
                        return -11;
                    }

                    await SavePreviewBufferToPngAsync(image_path_str, rgba, outWidth, outHeight);
                    return 0;
                }
                finally
                {
                    PreviewClose();
                }
            }

            public static int PreviewOpenSession(string sourcePath, float confThreshold, string copyrightImagePath)
            {
                PrepareStatusTracking();

                var previewParams = new NativePreviewParams
                {
                    file_path = sourcePath,
                    model_path = ResolveModelPath(),
                    copyright_image_path = copyrightImagePath ?? string.Empty,
                    input_width = 640,
                    input_height = 640,
                    conf_threshold = confThreshold,
                    iou_threshold = 0.45f,
                };

                return PreviewOpen(ref previewParams);
            }

            public static int PreviewUpdateMask(
                RectInfo[] rects,
                int count,
                ColorInfo nameColor,
                ColorInfo fixframeColor,
                bool copyright,
                string blackedOut,
                string fixedFrame,
                int blackedout_param,
                int fixedFrame_param,
                int copyrightOffsetX,
                int copyrightOffsetY,
                float copyrightScale,
                bool excludeByNameEnabled = false,
                int ocrExpandPixels = 2,
                int ocrMaxRoisPerFrame = 6,
                float textSimilarityThreshold = 0.85f,
                string maskExcludeTextCsv = "")
            {
                var nativeRects = new RectInfo[64];
                var rectCount = Math.Min(count, nativeRects.Length);
                Array.Copy(rects, nativeRects, rectCount);

                var maskParams = new NativePreviewMaskParams
                {
                    blacked_type = MaskTypeToNative(GetMaskTypeKind(blackedOut)),
                    name_color = nameColor,
                    blackedout_param = blackedout_param,
                    fixmask_type = MaskTypeToNative(GetMaskTypeKind(fixedFrame)),
                    fixframe_color = fixframeColor,
                    fixedFrame_param = fixedFrame_param,
                    fixed_rect_count = rectCount,
                    fixed_rects = nativeRects,
                    enable_copyright = copyright,
                    copyright_offset_x = copyrightOffsetX,
                    copyright_offset_y = copyrightOffsetY,
                    copyright_scale = copyrightScale,
                    exclude_by_name_enabled = excludeByNameEnabled,
                    ocr_expand_pixels = ocrExpandPixels,
                    ocr_max_rois_per_frame = ocrMaxRoisPerFrame,
                    text_similarity_threshold = textSimilarityThreshold,
                    mask_exclude_text_csv = maskExcludeTextCsv ?? string.Empty,
                    reserved = new int[4],
                };

                return PreviewUpdateParams(ref maskParams);
            }

            public static int PreviewGetFrameBuffer(int frameIndex, byte[] bgraBuffer, out int outWidth, out int outHeight)
            {
                return PreviewGetFrame(frameIndex, bgraBuffer, bgraBuffer.Length, out outWidth, out outHeight);
            }

            public static void PreviewCloseSessionSafe()
            {
                try
                {
                    PreviewClose();
                }
                catch (Exception ex) when (ex is DllNotFoundException || ex is EntryPointNotFoundException)
                {
                    Debug.WriteLine($"PreviewClose unavailable: {ex.Message}");
                }
            }

            public static void SetCopyrightOffset(int offsetX, int offsetY)
            {
                try
                {
                    PreviewSetCopyrightOffset(offsetX, offsetY);
                }
                catch (Exception ex)
                {
                    Debug.WriteLine($"SetCopyrightOffset failed: {ex.Message}");
                }
            }
        }

        private RectInfo[] BuildRectInfos()
        {
            return savedRects.Select(rect => new RectInfo
            {
                x = (int)rect.X,
                y = (int)rect.Y,
                width = (int)rect.Width,
                height = (int)rect.Height
            }).ToArray();
        }

        private (ColorInfo nameColor, ColorInfo fixedColor) BuildMaskColors()
        {
            SolidColorBrush blackedOutBrush = (SolidColorBrush)BlackedOut_color_icon.Foreground;
            Color blackedOutColor = blackedOutBrush.Color;
            ColorInfo blackedOutColorInfo = new ColorInfo
            {
                r = blackedOutColor.R,
                g = blackedOutColor.G,
                b = blackedOutColor.B
            };

            SolidColorBrush fixedFrameBrush = (SolidColorBrush)FixedFrame_color_icon.Foreground;
            Color fixedFrameColor = fixedFrameBrush.Color;
            ColorInfo fixedFrameColorInfo = new ColorInfo
            {
                r = fixedFrameColor.R,
                g = fixedFrameColor.G,
                b = fixedFrameColor.B
            };

            return (blackedOutColorInfo, fixedFrameColorInfo);
        }

        private void StopRealtimePreviewSession(bool skipNativeClose = false)
        {
            previewTimer?.Stop();
            previewLayoutInitialized = false;
            copyrightDragging = false;
            if (previewSessionOpen)
            {
                bool shouldSkipNativeClose = skipNativeClose || isWindowClosing || previewTickBusy;
                previewSessionOpen = false;

                if (!shouldSkipNativeClose)
                {
                    try
                    {
                        FrameProcessor.PreviewCloseSessionSafe();
                    }
                    catch (Exception ex)
                    {
                        Debug.WriteLine($"PreviewCloseSessionSafe failed: {ex.Message}");
                    }
                }
            }
        }

        private void PauseRealtimePreviewPlayback()
        {
            previewTimer.Stop();
            copyrightDragging = false;
        }

        private async Task<bool> StartRealtimePreviewSessionAsync(string sourcePath)
        {
            StopRealtimePreviewSession();

            float confThreshold = GetYoloThreshold();

            int openResult = await Task.Run(() => FrameProcessor.PreviewOpenSession(sourcePath, confThreshold, copyrightImagePath ?? string.Empty));
            if (openResult != 0)
            {
                Debug.WriteLine($"PreviewOpenSession failed: {openResult}");
                return false;
            }

            previewSessionOpen = true;
            previewFrameIndex = GetTargetFrameIndexFromSlider();
            int fpsValue = Math.Max(1, v_fps);
            previewFrameStep = Math.Max(1, fpsValue / 30);
            previewTimer.Interval = TimeSpan.FromMilliseconds(Math.Max(20, 1000.0 / Math.Min(30, fpsValue)));
            double previewHeight = image_preview.ActualHeight > 0 ? image_preview.ActualHeight : image_preview.Height;
            if (previewHeight <= 0)
            {
                previewHeight = 800;
            }
            scaleFactor = previewHeight / Math.Max(1, v_height);

            return true;
        }

        private int GetTargetFrameIndexFromSlider()
        {
            int fpsValue = Math.Max(1, v_fps);
            int totalFrames = Math.Max(1, v_nb_frames);
            int targetFrame = Math.Max(0, (int)Math.Floor(FrameSlideBar.Value * fpsValue));
            return Math.Min(totalFrames - 1, targetFrame);
        }

        private async void PreviewTimer_Tick(object? sender, object e)
        {
            if (isWindowClosing || previewTickBusy || !previewSessionOpen || running_state)
            {
                return;
            }

            previewTickBusy = true;
            try
            {
                var rectInfos = BuildRectInfos();
                var (nameColor, fixedColor) = BuildMaskColors();
                string blackedOut = GetComboText(BlackedOut_ComboBox, "Solid");
                string fixedFrame = GetComboText(FixedFrame_ComboBox, "Solid");
                int blackedOutParam = (int)BlackedOutSlideBar.Value;
                int fixedFrameParam = (int)FixedFrameSlideBar.Value;
                bool addCopyright = Add_Copyright.IsChecked.Value;

                int updateResult = await Task.Run(() => FrameProcessor.PreviewUpdateMask(
                    rectInfos,
                    rectInfos.Length,
                    nameColor,
                    fixedColor,
                    addCopyright,
                    blackedOut,
                    fixedFrame,
                    blackedOutParam,
                    fixedFrameParam,
                    copyrightOffsetX,
                    copyrightOffsetY,
                    (float)copyrightZoomScale,
                    excludeByNameEnabled,
                    ocrExpandPixels,
                    ocrMaxRoisPerFrame,
                    GetTextSimilarityThreshold(),
                    maskExcludeTextCsv));

                if (isWindowClosing || !previewSessionOpen)
                {
                    return;
                }

                if (updateResult != 0)
                {
                    return;
                }

                int expectedWidth = Math.Max(1, v_width);
                int expectedHeight = Math.Max(1, v_height);
                byte[] bgra = new byte[expectedWidth * expectedHeight * 4];

                var frameResult = await Task.Run(() =>
                {
                    int result = FrameProcessor.PreviewGetFrameBuffer(previewFrameIndex, bgra, out int outWidth, out int outHeight);
                    return (result, outWidth, outHeight);
                });

                if (isWindowClosing || !previewSessionOpen)
                {
                    return;
                }

                if (frameResult.result != 0)
                {
                    return;
                }

                int outWidth2 = frameResult.outWidth;
                int outHeight2 = frameResult.outHeight;
                if (outWidth2 <= 0 || outHeight2 <= 0)
                {
                    return;
                }

                int required = outWidth2 * outHeight2 * 4;
                if (bgra.Length < required)
                {
                    bgra = new byte[required];
                    int retry = FrameProcessor.PreviewGetFrameBuffer(previewFrameIndex, bgra, out outWidth2, out outHeight2);
                    if (retry != 0)
                    {
                        return;
                    }
                }

                lock (previewFrameCacheLock)
                {
                    lastPreviewFrameBgra = new byte[required];
                    Buffer.BlockCopy(bgra, 0, lastPreviewFrameBgra, 0, required);
                    lastPreviewFrameWidth = outWidth2;
                    lastPreviewFrameHeight = outHeight2;
                }

                if (previewBitmap == null || previewBitmap.PixelWidth != outWidth2 || previewBitmap.PixelHeight != outHeight2)
                {
                    previewBitmap = new WriteableBitmap(outWidth2, outHeight2);
                    image_preview.Source = previewBitmap;
                    previewLayoutInitialized = false;
                }

                if (!previewLayoutInitialized)
                {
                    double previewHeight = image_preview.ActualHeight > 0 ? image_preview.ActualHeight : image_preview.Height;
                    if (previewHeight <= 0)
                    {
                        previewHeight = 800;
                    }

                    scaleFactor = previewHeight / outHeight2;

                    int logicalWidth = 800 + (int)(outWidth2 * scaleFactor) - 280;
                    int logicalHeight = 850;
                    TryResizeAppWindow(logicalWidth, logicalHeight);
                    previewLayoutInitialized = true;
                }

                using (var pixelStream = previewBitmap.PixelBuffer.AsStream())
                {
                    pixelStream.Position = 0;
                    pixelStream.Write(bgra, 0, outWidth2 * outHeight2 * 4);
                }
                previewBitmap.Invalidate();

                int fpsValue = Math.Max(1, v_fps);
                double sliderSeconds = previewFrameIndex / (double)fpsValue;
                double sliderValue = Math.Clamp(sliderSeconds, FrameSlideBar.Minimum, FrameSlideBar.Maximum);
                suppressFrameSliderValueChanged = true;
                FrameSlideBar.Value = sliderValue;
                suppressFrameSliderValueChanged = false;

                int totalFrames = Math.Max(1, v_nb_frames);
                previewFrameIndex = (previewFrameIndex + previewFrameStep) % totalFrames;
            }
            catch (Exception ex)
            {
                Debug.WriteLine($"Realtime preview tick failed: {ex.Message}");
            }
            finally
            {
                previewTickBusy = false;
            }
        }

        private async void SaveImageButton_Click(object sender, Microsoft.UI.Xaml.RoutedEventArgs e)
        {
            var picker = new FileSavePicker();
            InitializeWithWindow.Initialize(picker, WindowNative.GetWindowHandle(this));
            picker.SuggestedStartLocation = PickerLocationId.PicturesLibrary;
            picker.FileTypeChoices.Add("PNG Image", new List<string>() { ".png" });
            picker.FileTypeChoices.Add("JPG Image", new List<string>() { ".jpg" });
            picker.DefaultFileExtension = ".png";
            picker.SuggestedFileName = "image";

            StorageFile file = await picker.PickSaveFileAsync();
            if (file != null)
            {
                try
                {
                    bool saved = false;
                    bool sourceIsVideo = PickAFileOutputTextBlock.Text.EndsWith(".mp4", StringComparison.OrdinalIgnoreCase);

                    if (sourceIsVideo)
                    {
                        saved = await TrySaveCurrentVideoPreviewFrameAsync(file);
                        if (!saved)
                        {
                            saved = await TrySaveLastPreviewFrameAsync(file);
                        }
                    }
                    else
                    {
                        saved = await TrySaveCurrentImagePreviewAsync(file);
                    }

                    if (!saved && !string.IsNullOrWhiteSpace(last_preview_image) && File.Exists(last_preview_image))
                    {
                        var sourceFile = await StorageFile.GetFileFromPathAsync(last_preview_image);
                        using (var sourceStream = await sourceFile.OpenAsync(FileAccessMode.Read))
                        using (var destinationStream = await file.OpenAsync(FileAccessMode.ReadWrite))
                        {
                            await sourceStream.AsStreamForRead().CopyToAsync(destinationStream.AsStreamForWrite());
                        }
                        saved = true;
                    }

                    if (!saved && previewSessionOpen)
                    {
                        bool wasPreviewRunning = previewTimer.IsEnabled;
                        if (wasPreviewRunning)
                        {
                            previewTimer.Stop();
                        }

                        try
                        {
                            saved = await TrySaveCurrentVideoPreviewFrameAsync(file);
                        }
                        finally
                        {
                            if (wasPreviewRunning && previewSessionOpen)
                            {
                                previewTimer.Start();
                            }
                        }
                    }

                    if (!saved)
                    {
                        InfoBar.Message = "No preview frame is available to save";
                        InfoBar.Severity = InfoBarSeverity.Informational;
                        InfoBar.IsOpen = true;
                        InfoBar.Visibility = Visibility.Visible;
                        return;
                    }

                    InfoBar.Message = "Preview image is saved";
                    InfoBar.Severity = InfoBarSeverity.Success;
                    InfoBar.IsOpen = true;
                    InfoBar.Visibility = Visibility.Visible;
                }
                catch (Exception ex)
                {
                    InfoBar.Message = "Save failed";
                    InfoBar.Severity = InfoBarSeverity.Error;
                    InfoBar.IsOpen = true;
                    InfoBar.Visibility = Visibility.Visible;
                }
            }
        }

        private async Task<bool> TrySaveLastPreviewFrameAsync(StorageFile file)
        {
            byte[]? bgra = null;
            int width = 0;
            int height = 0;

            lock (previewFrameCacheLock)
            {
                if (lastPreviewFrameBgra != null && lastPreviewFrameBgra.Length > 0 && lastPreviewFrameWidth > 0 && lastPreviewFrameHeight > 0)
                {
                    bgra = new byte[lastPreviewFrameBgra.Length];
                    Buffer.BlockCopy(lastPreviewFrameBgra, 0, bgra, 0, bgra.Length);
                    width = lastPreviewFrameWidth;
                    height = lastPreviewFrameHeight;
                }
            }

            if (bgra == null)
            {
                return false;
            }

            await SaveBgraToFileAsync(file, bgra, width, height);
            return true;
        }

        private static async Task SaveBgraToFileAsync(StorageFile file, byte[] bgra, int width, int height)
        {
            using var stream = await file.OpenAsync(FileAccessMode.ReadWrite);
            stream.Size = 0;

            string ext = System.IO.Path.GetExtension(file.Path).ToLowerInvariant();
            Guid encoderId = (ext == ".jpg" || ext == ".jpeg")
                ? BitmapEncoder.JpegEncoderId
                : BitmapEncoder.PngEncoderId;

            var encoder = await BitmapEncoder.CreateAsync(encoderId, stream);
            encoder.SetPixelData(
                BitmapPixelFormat.Bgra8,
                BitmapAlphaMode.Ignore,
                (uint)width,
                (uint)height,
                96,
                96,
                bgra);
            await encoder.FlushAsync();
        }

        private async Task<bool> TrySaveCurrentVideoPreviewFrameAsync(StorageFile file)
        {
            if (!previewSessionOpen)
            {
                return false;
            }

            var rectInfos = BuildRectInfos();
            var (nameColor, fixedColor) = BuildMaskColors();
            string blackedOut = GetComboText(BlackedOut_ComboBox, "Solid");
            string fixedFrame = GetComboText(FixedFrame_ComboBox, "Solid");
            bool addCopyright = Add_Copyright.IsChecked == true;
            int blackedOutParam = (int)BlackedOutSlideBar.Value;
            int fixedFrameParam = (int)FixedFrameSlideBar.Value;
            int offsetX = copyrightOffsetX;
            int offsetY = copyrightOffsetY;
            float scale = (float)copyrightZoomScale;

            int updateResult = await Task.Run(() => FrameProcessor.PreviewUpdateMask(
                rectInfos,
                rectInfos.Length,
                nameColor,
                fixedColor,
                addCopyright,
                blackedOut,
                fixedFrame,
                blackedOutParam,
                fixedFrameParam,
                offsetX,
                offsetY,
                scale,
                excludeByNameEnabled,
                ocrExpandPixels,
                ocrMaxRoisPerFrame,
                GetTextSimilarityThreshold(),
                maskExcludeTextCsv));

            if (updateResult != 0)
            {
                return false;
            }

            int expectedWidth = Math.Max(1, v_width);
            int expectedHeight = Math.Max(1, v_height);
            byte[] bgra = new byte[expectedWidth * expectedHeight * 4];

            var frameResult = await Task.Run(() =>
            {
                int result = FrameProcessor.PreviewGetFrameBuffer(previewFrameIndex, bgra, out int outWidth, out int outHeight);
                return (result, outWidth, outHeight);
            });

            if (frameResult.result != 0 || frameResult.outWidth <= 0 || frameResult.outHeight <= 0)
            {
                return false;
            }

            int required = frameResult.outWidth * frameResult.outHeight * 4;
            if (bgra.Length < required)
            {
                bgra = new byte[required];
                int retry = FrameProcessor.PreviewGetFrameBuffer(previewFrameIndex, bgra, out int outWidth, out int outHeight);
                if (retry != 0 || outWidth <= 0 || outHeight <= 0)
                {
                    return false;
                }
                frameResult = (retry, outWidth, outHeight);
            }

            await SaveBgraToFileAsync(file, bgra, frameResult.outWidth, frameResult.outHeight);
            return true;
        }

        private async Task<bool> TrySaveCurrentImagePreviewAsync(StorageFile file)
        {
            if (string.IsNullOrWhiteSpace(v_file_path) || !File.Exists(v_file_path))
            {
                return false;
            }

            string sourceExt = System.IO.Path.GetExtension(v_file_path).ToLowerInvariant();
            if (sourceExt != ".jpg" && sourceExt != ".jpeg" && sourceExt != ".png")
            {
                return false;
            }

            string tempDirectory = System.IO.Path.GetTempPath();
            string tempFileName = $"tmp_wol_save_{DateTime.Now:yyyyMMddHHmmssfff}.png";
            string tempOutputPath = System.IO.Path.Combine(tempDirectory, tempFileName);

            try
            {
                File.Copy(v_file_path, tempOutputPath, true);

                var rectInfos = BuildRectInfos();
                var (nameColor, fixedColor) = BuildMaskColors();
                bool addCopyright = Add_Copyright.IsChecked == true;
                string blackedOut = GetComboText(BlackedOut_ComboBox, "Solid");
                string fixedFrame = GetComboText(FixedFrame_ComboBox, "Solid");
                int blackedOutParam = (int)BlackedOutSlideBar.Value;
                int fixedFrameParam = (int)FixedFrameSlideBar.Value;

                int result = await FrameProcessor.Runpreview_apiAsync(
                    tempOutputPath,
                    rectInfos,
                    rectInfos.Length,
                    nameColor,
                    fixedColor,
                    addCopyright,
                    blackedOut,
                    fixedFrame,
                    blackedOutParam,
                    fixedFrameParam,
                    GetYoloThreshold(),
                    copyrightImagePath ?? string.Empty);

                if (result != 0 || !File.Exists(tempOutputPath))
                {
                    return false;
                }

                StorageFile processedFile = await StorageFile.GetFileFromPathAsync(tempOutputPath);
                using var processedStream = await processedFile.OpenReadAsync();
                var decoder = await BitmapDecoder.CreateAsync(processedStream);
                var pixelData = await decoder.GetPixelDataAsync(
                    BitmapPixelFormat.Bgra8,
                    BitmapAlphaMode.Ignore,
                    new BitmapTransform(),
                    ExifOrientationMode.IgnoreExifOrientation,
                    ColorManagementMode.DoNotColorManage);

                byte[] bgra = pixelData.DetachPixelData();
                await SaveBgraToFileAsync(file, bgra, (int)decoder.PixelWidth, (int)decoder.PixelHeight);
                return true;
            }
            finally
            {
                try
                {
                    if (File.Exists(tempOutputPath))
                    {
                        File.Delete(tempOutputPath);
                    }
                }
                catch
                {
                }
            }
        }

        private async void ShowColorDialog_Click(object sender, Microsoft.UI.Xaml.RoutedEventArgs e)
        {
            Button? clickedButton = sender as Button;
            string buttonName = clickedButton?.Name ?? "Unknown Button";

            StackPanel stack = new StackPanel();
            stack.Children.Clear();
            stack.Spacing = 5.0;

            // カラーピッカーを追加
            ColorPicker colorPicker = new ColorPicker();
            colorPicker.IsColorSpectrumVisible = true;
            colorPicker.ColorSpectrumShape = ColorSpectrumShape.Box;
            colorPicker.IsMoreButtonVisible = false;
            colorPicker.IsColorSliderVisible = true;
            colorPicker.IsColorChannelTextInputVisible = true;
            colorPicker.IsHexInputVisible = true;
            colorPicker.IsAlphaEnabled = false;
            colorPicker.IsAlphaSliderVisible = true;
            colorPicker.IsAlphaTextInputVisible = true;

            // ボタン内の FontIcon の色を取得し、初回なら白を選択するようにする
            if (clickedButton != null && clickedButton.Content is StackPanel stackPanel)
            {
                var fontIcon = stackPanel.Children.OfType<FontIcon>().FirstOrDefault();
                if (fontIcon != null && fontIcon.Foreground is SolidColorBrush brush)
                {
                    // 既定の色である黒の場合、初回は白 (Colors.White) を初期値にする
                    if (brush.Color == Microsoft.UI.Colors.Black)
                    {
                        colorPicker.Color = Microsoft.UI.Colors.White;
                    }
                    else
                    {
                        // 既にユーザーが色を選択している場合、その色を使用
                        colorPicker.Color = brush.Color;
                    }
                }
            }

            stack.Children.Add(colorPicker);

            // 新しい ContentDialog を作成
            ContentDialog newdialog = new ContentDialog()
            {
                Title = $"Select {buttonName}",
                Content = stack,
                PrimaryButtonText = "OK",
                CloseButtonText = "Cancel"
            };

            // XamlRoot を設定
            newdialog.XamlRoot = this.Content.XamlRoot;

            // ダイアログを表示
            var result = await newdialog.ShowAsync();

            if (result == ContentDialogResult.Primary)
            {
                // OK ボタンが押された場合の処理
                var selectedColor = colorPicker.Color;
                // 選択された色を呼び出し元のボタンのアイコンの色に設定
                if (clickedButton != null && clickedButton.Content is StackPanel sp)
                {
                    var fontIcon = sp.Children.OfType<FontIcon>().FirstOrDefault();
                    if (fontIcon != null)
                    {
                        fontIcon.Foreground = new SolidColorBrush(selectedColor);
                        FixedFrame_color_icon_ColorChanged();
                    }
                }
            }

        }
        private async Task<Dictionary<string, string>> GetVideoProperties(StorageFile file)
        {
            var ffprobePath = System.IO.Path.Combine(AppContext.BaseDirectory, "ffprobe.exe");
            var arguments = $"-v error -select_streams v:0 -show_entries stream=width,height,color_primaries,r_frame_rate,avg_frame_rate,nb_frames:format=duration -of default=nw=1 \"{file.Path}\"";

            var processStartInfo = new ProcessStartInfo
            {
                FileName = ffprobePath,
                Arguments = arguments,
                RedirectStandardOutput = true,
                UseShellExecute = false,
                CreateNoWindow = true
            };

            var process = new Process { StartInfo = processStartInfo };
            var output = new StringBuilder();

            process.OutputDataReceived += (sender, e) =>
            {
                if (!string.IsNullOrEmpty(e.Data))
                {
                    output.AppendLine(e.Data);
                }
            };

            process.Start();
            process.BeginOutputReadLine();
            await process.WaitForExitAsync();

            var properties = ParseFfprobeOutput(output.ToString());

            // 音声ストリームの有無を判定
            var audioArguments = $"-v error -select_streams a -show_entries stream=index -of default=nw=1:nk=1 \"{file.Path}\"";
            var audioProcessStartInfo = new ProcessStartInfo
            {
                FileName = ffprobePath,
                Arguments = audioArguments,
                RedirectStandardOutput = true,
                UseShellExecute = false,
                CreateNoWindow = true
            };

            var audioProcess = new Process { StartInfo = audioProcessStartInfo };
            var audioOutput = new StringBuilder();

            audioProcess.OutputDataReceived += (sender, e) =>
            {
                if (!string.IsNullOrEmpty(e.Data))
                {
                    audioOutput.AppendLine(e.Data);
                }
            };

            audioProcess.Start();
            audioProcess.BeginOutputReadLine();
            await audioProcess.WaitForExitAsync();

            // audioOutputが空でなければ音声あり
            bool hasAudio = !string.IsNullOrWhiteSpace(audioOutput.ToString());
            properties["has_audio"] = hasAudio.ToString().ToLower(); // "true" or "false"

            return properties;
        }

        private Dictionary<string, string> ParseFfprobeOutput(string output)
        {
            var properties = new Dictionary<string, string>();
            var lines = output.Split(new[] { '\r', '\n' }, StringSplitOptions.RemoveEmptyEntries);
            foreach (var line in lines)
            {
                int separatorIndex = line.IndexOf('=');
                if (separatorIndex <= 0 || separatorIndex >= line.Length - 1)
                {
                    continue;
                }

                string key = line.Substring(0, separatorIndex).Trim();
                string value = line.Substring(separatorIndex + 1).Trim();

                if (string.IsNullOrWhiteSpace(key) || string.IsNullOrWhiteSpace(value) || string.Equals(value, "N/A", StringComparison.OrdinalIgnoreCase))
                {
                    continue;
                }

                if (key == "r_frame_rate" || key == "avg_frame_rate")
                {
                    var parts = value.Split('/');
                    if (parts.Length == 2
                        && double.TryParse(parts[0], NumberStyles.Float, CultureInfo.InvariantCulture, out double numerator)
                        && double.TryParse(parts[1], NumberStyles.Float, CultureInfo.InvariantCulture, out double denominator)
                        && denominator > 0)
                    {
                        properties[key] = (numerator / denominator).ToString(CultureInfo.InvariantCulture);
                    }

                    continue;
                }

                properties[key] = value;
            }

            return properties;
        }
        private void UIControl_enable_false()
        {
            if (isWindowClosing)
            {
                return;
            }

            try
            {
                PickAFileButton.IsEnabled = false;
                BlackedOut_color.IsEnabled = false;
                FixedFrame_color.IsEnabled = false;
                FrameSlideBar.IsEnabled = false;
                Start_min.IsEnabled = false;
                Start_sec.IsEnabled = false;
                End_min.IsEnabled = false;
                End_sec.IsEnabled = false;
                PreviewButton.IsEnabled = false;
                SaveImageButton.IsEnabled = false;
                BlackedOutStartButton.IsEnabled = false;
                // ConvertButton.IsEnabled = false;  // TODO: ConvertButton not found in XAML
                Add_Copyright.IsEnabled = false;
                BlackedOut_ComboBox.IsEnabled = false;
                FixedFrame_ComboBox.IsEnabled = false;
                BlackedOutSlideBar.IsEnabled = false;
                FixedFrameSlideBar.IsEnabled = false;
                BitrateSlideBar.IsEnabled = false;
                ForX.IsEnabled = false;
                YoloThresholdSlider.IsEnabled = false;
                DisableAudio.IsEnabled = false;
                SelectCopyrightImageButton.IsEnabled = false;
                SetSliderToStartButton.IsEnabled = false;
                SetSliderToEndButton.IsEnabled = false;
                ExcludeByNameEnabledCheckBox.IsEnabled = false;
                OpenExcludeByNameSettingsButton.IsEnabled = false;
            }
            catch (ArgumentException)
            {
                isWindowClosing = true;
            }

        }
        private void UIControl_enable_true()
        {
            if (isWindowClosing)
            {
                return;
            }

            try
            {
                PickAFileButton.IsEnabled = true;
                if ((PickAFileOutputTextBlock.Text != "Operation cancelled.") && ((PickAFileOutputTextBlock.Text != "")))
                {
                    PreviewButton.IsEnabled = true;
                    SaveImageButton.IsEnabled = true;
                    string fileName = PickAFileOutputTextBlock.Text;
                    bool isVideoFile = fileName.EndsWith(".mp4", StringComparison.OrdinalIgnoreCase);

                    FrameSlideBar.IsEnabled = isVideoFile;
                    SetSliderToStartButton.IsEnabled = isVideoFile;
                    SetSliderToEndButton.IsEnabled = isVideoFile;

                    if (fileName.EndsWith(".mp4", StringComparison.OrdinalIgnoreCase))
                    {
                        BlackedOutStartButton.IsEnabled = true;
                    }
                    else
                    {
                        BlackedOutStartButton.IsEnabled = false;
                    }
                }
                else
                {
                    PreviewButton.IsEnabled = false;
                    BlackedOutStartButton.IsEnabled = false;
                    FrameSlideBar.IsEnabled = false;
                    SetSliderToStartButton.IsEnabled = false;
                    SetSliderToEndButton.IsEnabled = false;
                }
                BlackedOut_color.IsEnabled = true;
                FixedFrame_color.IsEnabled = true;
                Start_min.IsEnabled = true;
                Start_sec.IsEnabled = true;
                End_min.IsEnabled = true;
                End_sec.IsEnabled = true;
                Add_Copyright.IsEnabled = true;
                YoloThresholdSlider.IsEnabled = true;
                DisableAudio.IsEnabled = true;
                SelectCopyrightImageButton.IsEnabled = true;
                ExcludeByNameEnabledCheckBox.IsEnabled = true;
                OpenExcludeByNameSettingsButton.IsEnabled = excludeByNameEnabled;
                if (useblackedout == true)
                {
                    BlackedOut_ComboBox.IsEnabled = true;
                    FixedFrame_ComboBox.IsEnabled = true;
                    BlackedOutSlideBar.IsEnabled = true;
                    FixedFrameSlideBar.IsEnabled = true;
                    BitrateSlideBar.IsEnabled = true;
                }
                Start_End_min_sec_ValueChanged(null, null);
            }
            catch (ArgumentException)
            {
                isWindowClosing = true;
            }
        }

        private async void YoloThresholdSlider_ValueChanged(object sender, RangeBaseValueChangedEventArgs e)
        {
            if (previewThresholdUpdateBusy || !previewSessionOpen || running_state || string.IsNullOrWhiteSpace(v_file_path))
            {
                return;
            }

            previewThresholdUpdateBusy = true;
            try
            {
                bool wasPlaying = previewTimer.IsEnabled;
                int currentFrame = previewFrameIndex;

                bool reopened = await StartRealtimePreviewSessionAsync(v_file_path);
                if (!reopened)
                {
                    return;
                }

                previewFrameIndex = GetTargetFrameIndexFromSlider();

                if (wasPlaying)
                {
                    previewTimer.Start();
                }
                else
                {
                    await PreviewSingleFrameAsync(previewFrameIndex, true);
                }
            }
            finally
            {
                previewThresholdUpdateBusy = false;
            }
        }

        // スライダーの値が変更されたときに呼ばれるイベントハンドラ
        private async void FrameSlideBar_ValueChanged(object sender, RangeBaseValueChangedEventArgs e)
        {
            int currentSeconds = Math.Max(0, (int)Math.Round(e.NewValue));
            int n_min = currentSeconds / 60;
            int n_mod_sec = currentSeconds % 60;
            FrameTextBlock_n.Text = $"{n_min}:{n_mod_sec:D2}";

            if (suppressFrameSliderValueChanged || running_state)
            {
                return;
            }

            if (!previewSessionOpen &&
                !previewSessionAutoOpenBusy &&
                !string.IsNullOrWhiteSpace(v_file_path) &&
                v_file_path.EndsWith(".mp4", StringComparison.OrdinalIgnoreCase))
            {
                previewSessionAutoOpenBusy = true;
                try
                {
                    bool opened = await StartRealtimePreviewSessionAsync(v_file_path);
                    if (!opened)
                    {
                        return;
                    }
                }
                finally
                {
                    previewSessionAutoOpenBusy = false;
                }
            }

            if (previewSessionOpen)
            {
                previewFrameIndex = GetTargetFrameIndexFromSlider();
                if (!suppressPreviewFrameSliderRefresh)
                {
                    await RefreshCurrentPreviewFrameIfPausedAsync();
                }
            }
        }
        private async void AboutButton_Click(object sender, Microsoft.UI.Xaml.RoutedEventArgs e)
        {
            var packageVersion = Package.Current.Id.Version;
            var versionText = $"Version: {packageVersion.Major}.{packageVersion.Minor}.{packageVersion.Build}.{packageVersion.Revision}";

            var aboutDialog = new ContentDialog
            {
                Content = new StackPanel
                {
                    Children =
                    {
                        new TextBlock { Text = "WoLNamesBlackedOut", FontSize=20, Margin = new Microsoft.UI.Xaml.Thickness(12, 0, 0, 12) ,HorizontalAlignment = HorizontalAlignment.Center},
                        new TextBlock { Text = versionText, FontSize=16,Margin = new Microsoft.UI.Xaml.Thickness(0, 0, 0, 12) ,HorizontalAlignment = HorizontalAlignment.Center},
                        new HyperlinkButton { Content = "Calocen Rieti(Twitter)", NavigateUri = new Uri("https://x.com/calcMCalcm"), Margin = new Microsoft.UI.Xaml.Thickness(0, 0, 0, 10) ,HorizontalAlignment = HorizontalAlignment.Center },
                        new HyperlinkButton { Content = "Support Site", NavigateUri = new Uri("https://blog.calocenrieti.com/blog/wol_names_blacked_out_win/"), Margin = new Microsoft.UI.Xaml.Thickness(0, 0, 0, 10) ,HorizontalAlignment = HorizontalAlignment.Center},
                        new HyperlinkButton { Content = "Discord", NavigateUri = new Uri("https://discord.gg/q2Hqr4tD8v"), Margin = new Microsoft.UI.Xaml.Thickness(0, 0, 0, 10) ,HorizontalAlignment = HorizontalAlignment.Center},
                        new HyperlinkButton { Content = "GitHub", NavigateUri = new Uri("https://github.com/calocenrieti/WoLNamesBlackedOutWin"), Margin = new Microsoft.UI.Xaml.Thickness(0, 0, 0, 12) ,HorizontalAlignment = HorizontalAlignment.Center},
                        new HyperlinkButton { Content = "Donate", NavigateUri = new Uri("https://buymeacoffee.com/calocenrieti"), Margin = new Microsoft.UI.Xaml.Thickness(0, 0, 0, 12) ,HorizontalAlignment = HorizontalAlignment.Center},
                        new TextBlock { Text = "This software uses FFmpeg licensed under the LGPLv2 \nand its source can be downloaded https://github.com/FFmpeg/FFmpeg.git", FontSize=12,Margin = new Microsoft.UI.Xaml.Thickness(0, 0, 0, 8) ,HorizontalAlignment = HorizontalAlignment.Center,TextWrapping=TextWrapping.Wrap}
                    }
                },
                CloseButtonText = "Close"
            };

            aboutDialog.XamlRoot = this.Content.XamlRoot;
            await aboutDialog.ShowAsync();
        }
        private async void LicenseButton_Click(object sender, Microsoft.UI.Xaml.RoutedEventArgs e)
        {
            var aboutDialog = new ContentDialog
            {
                Content = new ScrollViewer()
                {
                    Content = new StackPanel
                    {
                        Children =
                        {
                            //c++
                            new HyperlinkButton { Content = "FFmpeg 9.01", NavigateUri = new Uri("http://www.gnu.org/licenses/old-licenses/lgpl-2.1.html"), Margin = new Microsoft.UI.Xaml.Thickness(0, 0, 0, 10) ,HorizontalAlignment = HorizontalAlignment.Center },
                            new HyperlinkButton { Content = "ByteTrack-cpp", NavigateUri = new Uri("https://github.com/derpda/ByteTrack-cpp/blob/main/LICENSE"), Margin = new Microsoft.UI.Xaml.Thickness(0, 0, 0, 10) ,HorizontalAlignment = HorizontalAlignment.Center },
                            new HyperlinkButton { Content = "Eigen 5.0.0", NavigateUri = new Uri("https://gitlab.com/libeigen/eigen/-/blob/master/COPYING.APACHE"), Margin = new Microsoft.UI.Xaml.Thickness(0, 0, 0, 10) ,HorizontalAlignment = HorizontalAlignment.Center },
                            new HyperlinkButton { Content = "Microsoft.Windows.AI.MachineLearning 2.2.12", NavigateUri = new Uri("https://www.nuget.org/packages/Microsoft.Windows.AI.MachineLearning/2.2.12/license"), Margin = new Microsoft.UI.Xaml.Thickness(0, 0, 0, 10) ,HorizontalAlignment = HorizontalAlignment.Center },
                            new HyperlinkButton { Content = "Microsoft.Windows.CppWinRT 3.0.260715.1", NavigateUri = new Uri("https://www.nuget.org/packages/Microsoft.Windows.CppWinRT/3.0.260715.1/license"), Margin = new Microsoft.UI.Xaml.Thickness(0, 0, 0, 10) ,HorizontalAlignment = HorizontalAlignment.Center },
                            //c#
                            new HyperlinkButton { Content = "Microsoft.Windows.SDK.BuildTools 10.0.28000.2526", NavigateUri = new Uri("https://aka.ms/WinSDKLicenseURL"), Margin = new Microsoft.UI.Xaml.Thickness(0, 0, 0, 10) ,HorizontalAlignment = HorizontalAlignment.Center },
                            new HyperlinkButton { Content = "Microsoft.WindowsAppSDK 2.3.1", NavigateUri = new Uri("https://www.nuget.org/packages/Microsoft.WindowsAppSDK/2.3.1/license"), Margin = new Microsoft.UI.Xaml.Thickness(0, 0, 0, 10) ,HorizontalAlignment = HorizontalAlignment.Center },
                            new HyperlinkButton { Content = "Microsoft.Windows.SDK.BuildTools.MSI 1.7.20250829.1", NavigateUri = new Uri("https://www.nuget.org/packages/Microsoft.WindowsSDK.BuildTools.MSIX/1.7.20250829.1/license"), Margin = new Microsoft.UI.Xaml.Thickness(0, 0, 0, 10) ,HorizontalAlignment = HorizontalAlignment.Center },
                            new HyperlinkButton { Content = "Microsoft.Windows.AI.MachineLearning 2.1.74", NavigateUri = new Uri("https://www.nuget.org/packages/Microsoft.Windows.AI.MachineLearning/2.1.74/license"), Margin = new Microsoft.UI.Xaml.Thickness(0, 0, 0, 10) ,HorizontalAlignment = HorizontalAlignment.Center },
                            new HyperlinkButton { Content = "Microsoft.WindowsAppSDK.AI 2.3.4", NavigateUri = new Uri("https://www.nuget.org/packages/Microsoft.WindowsAppSDK.AI/2.3.4/license"), Margin = new Microsoft.UI.Xaml.Thickness(0, 0, 0, 10) ,HorizontalAlignment = HorizontalAlignment.Center },
                            new HyperlinkButton { Content = "Microsoft.WindowsAppSDK.Base 2.0.4", NavigateUri = new Uri("https://www.nuget.org/packages/Microsoft.WindowsAppSDK.Base/2.0.4/license"), Margin = new Microsoft.UI.Xaml.Thickness(0, 0, 0, 10) ,HorizontalAlignment = HorizontalAlignment.Center },
                            new HyperlinkButton { Content = "Microsoft.WindowsAppSDK.DWrite 2.1.0", NavigateUri = new Uri("https://www.nuget.org/packages/Microsoft.WindowsAppSDK.DWrite/2.1.0/license"), Margin = new Microsoft.UI.Xaml.Thickness(0, 0, 0, 10) ,HorizontalAlignment = HorizontalAlignment.Center },
                            new HyperlinkButton { Content = "Microsoft.WindowsAppSDK.Foundation 2.3.5", NavigateUri = new Uri("https://www.nuget.org/packages/Microsoft.WindowsAppSDK.Foundation/2.3.5/license"), Margin = new Microsoft.UI.Xaml.Thickness(0, 0, 0, 10) ,HorizontalAlignment = HorizontalAlignment.Center },
                            new HyperlinkButton { Content = "Microsoft.WindowsAppSDK.InteractiveExperien 2.1.3", NavigateUri = new Uri("https://www.nuget.org/packages/Microsoft.WindowsAppSDK.InteractiveExperiences/2.1.3/license"), Margin = new Microsoft.UI.Xaml.Thickness(0, 0, 0, 10) ,HorizontalAlignment = HorizontalAlignment.Center },
                            new HyperlinkButton { Content = "Microsoft.WindowsAppSDK.ML 2.1.74", NavigateUri = new Uri("https://www.nuget.org/packages/Microsoft.WindowsAppSDK.ML/2.1.74/license"), Margin = new Microsoft.UI.Xaml.Thickness(0, 0, 0, 10) ,HorizontalAlignment = HorizontalAlignment.Center },
                            new HyperlinkButton { Content = "Microsoft.WindowsAppSDK.Runtime 2.3.1", NavigateUri = new Uri("https://www.nuget.org/packages/Microsoft.WindowsAppSDK.Runtime/2.3.1/license"), Margin = new Microsoft.UI.Xaml.Thickness(0, 0, 0, 10) ,HorizontalAlignment = HorizontalAlignment.Center },
                            new HyperlinkButton { Content = "Microsoft.WindowsAppSDK.Widgets 2.0.5", NavigateUri = new Uri("https://www.nuget.org/packages/Microsoft.WindowsAppSDK.Widgets/2.0.5/license"), Margin = new Microsoft.UI.Xaml.Thickness(0, 0, 0, 10) ,HorizontalAlignment = HorizontalAlignment.Center },
                            new HyperlinkButton { Content = "Microsoft.WindowsAppSDK.WinUI 2.3.0", NavigateUri = new Uri("https://www.nuget.org/packages/Microsoft.WindowsAppSDK.WinUI/2.3.0/license"), Margin = new Microsoft.UI.Xaml.Thickness(0, 0, 0, 10) ,HorizontalAlignment = HorizontalAlignment.Center },
                            new HyperlinkButton { Content = "Microsoft.Web.WebView2 1.0.4129.50", NavigateUri = new Uri("https://www.nuget.org/packages/Microsoft.Web.WebView2/1.0.4129.50/license"), Margin = new Microsoft.UI.Xaml.Thickness(0, 0, 0, 10) ,HorizontalAlignment = HorizontalAlignment.Center },
                            new HyperlinkButton { Content = "System.Numerics.Tensors 9.0.0", NavigateUri = new Uri("https://licenses.nuget.org/MIT"), Margin = new Microsoft.UI.Xaml.Thickness(0, 0, 0, 10) ,HorizontalAlignment = HorizontalAlignment.Center },
                            //model
                            new HyperlinkButton { Content = "PaddleOCR (model)", NavigateUri = new Uri("https://github.com/PaddlePaddle/PaddleOCR?tab=Apache-2.0-1-ov-file"), Margin = new Microsoft.UI.Xaml.Thickness(0, 0, 0, 10) ,HorizontalAlignment = HorizontalAlignment.Center },
                            new HyperlinkButton { Content = "Ultralytics 8.4.7 (model)", NavigateUri = new Uri("https://www.ultralytics.com/legal/agpl-3-0-software-license"), Margin = new Microsoft.UI.Xaml.Thickness(0, 0, 0, 10) ,HorizontalAlignment = HorizontalAlignment.Center },
                        }
                    }
                },
                CloseButtonText = "Close"
            };

            aboutDialog.XamlRoot = this.Content.XamlRoot;
            await aboutDialog.ShowAsync();
        }

        private async Task StartPreviewAsync()
        {
            SafeSetProgressIndeterminate(true);
            await WaitForProgressIndicatorAsync();

            //PreviewButton.IsEnabled = false;
            UIControl_enable_false();
            PreviewButton.IsEnabled = true;
            running_state = true;

            StopRealtimePreviewSession();

            // 現在の日時を使用してユニークなファイル名を作成し、一時ディレクトリに保存
            string tempDirectory = System.IO.Path.GetTempPath();
            string fileName = $"tmp_wol_{DateTime.Now:yyyyMMddHHmmssfff}.png";
            string outputPath = System.IO.Path.Combine(tempDirectory, fileName);
            string PickAFileOutputTextBlock_text = PickAFileOutputTextBlock.Text;

            if (PickAFileOutputTextBlock_text.EndsWith(".mp4", StringComparison.OrdinalIgnoreCase)) //動画の時
            {
                BeginPreviewStatusFeedback(GetLocalizedString("Runtime.PreviewInitializing", "Preparing preview..."));
                bool opened = false;
                try
                {
                    opened = await StartRealtimePreviewSessionAsync(v_file_path);
                }
                finally
                {
                    EndPreviewStatusFeedback(opened
                        ? GetLocalizedString("Runtime.PreviewReady", "Preview ready")
                        : GetLocalizedString("Runtime.PreviewInitFailed", "Preview initialization failed"));
                }
                if (!opened)
                {
                    InfoBar.Message = "Preview initialization failed";
                    InfoBar.Severity = InfoBarSeverity.Error;
                    InfoBar.IsOpen = true;
                    InfoBar.Visibility = Visibility.Visible;
                    SafeSetProgressIndeterminate(false);
                    UIControl_enable_true();
                    running_state = false;
                    suppressPreviewToggleEvent = true;
                    PreviewButton.IsChecked = false;
                    suppressPreviewToggleEvent = false;
                    SetPreviewButtonVisual(false);
                    return;
                }

                previewTimer.Start();
                last_preview_image = string.Empty;
                SaveImageButton.IsEnabled = true;
                SafeSetProgressIndeterminate(false);
                UIControl_enable_true();
                running_state = false;
                RemoveAllRectangles();
                return;
            }
            else //静止画の時
            {
                File.Copy(v_file_path, outputPath, true);
            }
            last_preview_image = outputPath;

            // RectInfo 配列を作成
            RectInfo[] rectInfos = savedRects.Select(rect => new RectInfo
            {
                x = (int)rect.X,
                y = (int)rect.Y,
                width = (int)rect.Width,
                height = (int)rect.Height
            }).ToArray();

            //int count = rectInfos.Length;

            // 色情報を定義（BlackedOut_color_iconの色を取得）
            SolidColorBrush BlackedOut_color_icon_brush = (SolidColorBrush)BlackedOut_color_icon.Foreground;
            Color BlackedOut_color_icon_color = BlackedOut_color_icon_brush.Color;
            ColorInfo BlackedOut_color_icon_color_info = new ColorInfo
            {
                r = BlackedOut_color_icon_color.R,
                g = BlackedOut_color_icon_color.G,
                b = BlackedOut_color_icon_color.B
            };

            // 色情報を定義（FixedFrame_color_iconの色を取得）
            SolidColorBrush FixedFrame_color_icon_brush = (SolidColorBrush)FixedFrame_color_icon.Foreground;
            Color FixedFrame_color_icon_color = FixedFrame_color_icon_brush.Color;
            ColorInfo FixedFrame_color_icon_color_Info = new ColorInfo
            {
                r = FixedFrame_color_icon_color.R,
                g = FixedFrame_color_icon_color.G,
                b = FixedFrame_color_icon_color.B
            };

            BeginPreviewStatusFeedback(GetLocalizedString("Runtime.PreviewInitializing", "Preparing preview..."));
            bool previewApiSucceeded = false;
            try
            {
                await FrameProcessor.Runpreview_apiAsync(outputPath, rectInfos, rectInfos.Length, BlackedOut_color_icon_color_info, FixedFrame_color_icon_color_Info, Add_Copyright.IsChecked.Value, GetComboText(BlackedOut_ComboBox, "Solid"), GetComboText(FixedFrame_ComboBox, "Solid"), (int)BlackedOutSlideBar.Value, (int)FixedFrameSlideBar.Value, GetYoloThreshold(), copyrightImagePath ?? string.Empty);
                previewApiSucceeded = true;
            }
            finally
            {
                EndPreviewStatusFeedback(previewApiSucceeded
                    ? GetLocalizedString("Runtime.PreviewReady", "Preview ready")
                    : GetLocalizedString("Runtime.PreviewInitFailed", "Preview initialization failed"));
            }

            // 保存された PNG ファイルを Image コントロールに表示
            var bitmapImage = new BitmapImage(new Uri(outputPath));

            bitmapImage.ImageOpened += (s, ev) =>
            {
                if (isWindowClosing)
                {
                    return;
                }

                var originalHeight = bitmapImage.PixelHeight;
                scaleFactor = image_preview.Height / originalHeight;
                // 縮小率を表示
                Debug.WriteLine($"縮小率: {scaleFactor}");

                int logicalWidth = 800 + (int)(bitmapImage.PixelWidth * scaleFactor) - 280;
                int logicalHeight = 850;
                TryResizeAppWindow(logicalWidth, logicalHeight);


                image_preview.Source = bitmapImage;
                SafeSetProgressIndeterminate(false);
                //PreviewButton.IsEnabled = true;
                UIControl_enable_true();
                PreviewButton.IsEnabled = true;
                running_state = false;
                suppressPreviewToggleEvent = true;
                PreviewButton.IsChecked = false;
                suppressPreviewToggleEvent = false;
                SetPreviewButtonVisual(false);

                // 描画されている矩形を全て消去
                RemoveAllRectangles();

            };

            // BitmapImage の読み込みを開始
            image_preview.Source = bitmapImage;
            SaveImageButton.IsEnabled = true;
        }

        private async void PreviewButton_Checked(object sender, RoutedEventArgs e)
        {
            if (PreviewButton.Visibility != Visibility.Visible)
            {
                return;
            }

            if (suppressPreviewToggleEvent)
            {
                return;
            }

            SetPreviewButtonVisual(true);
            if (previewSessionOpen)
            {
                return;
            }

            await StartPreviewAsync();
        }

        private void PreviewButton_Unchecked(object sender, RoutedEventArgs e)
        {
            if (PreviewButton.Visibility != Visibility.Visible)
            {
                return;
            }

            if (suppressPreviewToggleEvent)
            {
                return;
            }

            PauseRealtimePreviewPlayback();
            SetPreviewButtonVisual(false);
            running_state = false;
            SafeSetProgressIndeterminate(false);
            UIControl_enable_true();
        }


        private async Task ExtractFrameToPng(string videoFilePath, int frameNumber, string outputPath)
        {
            // HDR動画（bt2020）の場合のフィルタ
            string hdrFilter = "zscale=t=linear:npl=100,format=gbrpf32le,zscale=p=bt709,tonemap=tonemap=hable:desat=0,zscale=t=bt709:m=bt709:r=tv";

            string arguments = "";
            if (v_color_primaries == "bt2020")
            {
                arguments = $"-hwaccel \"{hwaccel}\" -ss {frameNumber} -i \"{videoFilePath}\" -vf \"{hdrFilter}\" -frames:v 1 -q:v 2 \"{outputPath}\"";
            }
            else
            {
                arguments = $"-hwaccel \"{hwaccel}\" -ss \"{frameNumber}\" -i \"{videoFilePath}\" -vsync vfr -q:v 2 \"{outputPath}\"";
            }


            var processStartInfo = new ProcessStartInfo
            {
                FileName = ffmpegPath,
                Arguments = arguments,
                RedirectStandardOutput = true,
                RedirectStandardError = true,
                UseShellExecute = false,
                CreateNoWindow = true
            };

            var process = new Process { StartInfo = processStartInfo };
            var output = new StringBuilder();
            var error = new StringBuilder();

            process.OutputDataReceived += (sender, e) =>
            {
                if (!string.IsNullOrEmpty(e.Data))
                {
                    output.AppendLine(e.Data);
                }
            };

            process.ErrorDataReceived += (sender, e) =>
            {
                if (!string.IsNullOrEmpty(e.Data))
                {
                    error.AppendLine(e.Data);
                }
            };

            process.Start();
            process.BeginOutputReadLine();
            process.BeginErrorReadLine();
            await process.WaitForExitAsync();

            // エラーメッセージを表示
            if (error.Length > 0)
            {
                Debug.WriteLine("ffmpeg error: " + error.ToString());
            }
        }

        private async Task StartBlackedOutAsync()
        {
            if (isWindowClosing)
            {
                return;
            }

            StopRealtimePreviewSession();
            suppressPreviewToggleEvent = true;
            PreviewButton.IsChecked = false;
            suppressPreviewToggleEvent = false;
            SetPreviewButtonVisual(false);
            //開始時刻が終了時刻より大きければスタートしない
            if (Start_min.Value * 60 + Start_sec.Value >= End_min.Value * 60 + End_sec.Value)
            {
                InfoBar.Message = "Error:Start_min + Start_sec >= End_min + End_sec.";
                InfoBar.Severity = InfoBarSeverity.Error;
                InfoBar.IsOpen = true;
                InfoBar.Visibility = Visibility.Visible;
            }
            else
            {
                UIControl_enable_false();
                BlackedOutStartButton.IsEnabled = true;
                running_state = true;
                string tempDirectory = System.IO.Path.GetTempPath();
                string video_temp_filename_2 = System.IO.Path.Combine(tempDirectory, $"tmp_wol_{DateTime.Now:yyyyMMddHHmmssfff}_2.mp4");

                int mod_frame_sec;
                int mod_frame = (int)(v_nb_frames % v_fps);
                if (mod_frame > 0)
                {
                    mod_frame_sec = 1;
                }
                else
                {
                    mod_frame_sec = 0;
                }

                int all_sec = (int)FrameSlideBar.Value + mod_frame_sec;

                int start_time = (int)(Start_min.Value * 60) + (int)(Start_sec.Value);
                int end_time = (int)(End_min.Value * 60) + (int)(End_sec.Value);
                v_start_time = start_time;
                v_end_time = end_time;
                bool disableAudio = DisableAudio?.IsChecked == true;

                string v_bitrate = ((int)BitrateSlideBar.Value).ToString() + "M";

                string video_temp_filename_1 = v_file_path;
                FFMpeg_text.Text = "";

                // RectInfo 配列を作成
                RectInfo[] rectInfos = savedRects.Select(rect => new RectInfo
                {
                    x = (int)rect.X,
                    y = (int)rect.Y,
                    width = (int)rect.Width,
                    height = (int)rect.Height
                }).ToArray();


                // 色情報を定義（BlackedOut_color_iconの色を取得）
                SolidColorBrush BlackedOut_color_icon_brush = (SolidColorBrush)BlackedOut_color_icon.Foreground;
                Color BlackedOut_color_icon_color = BlackedOut_color_icon_brush.Color;
                ColorInfo BlackedOut_color_icon_color_info = new ColorInfo
                {
                    r = BlackedOut_color_icon_color.R,
                    g = BlackedOut_color_icon_color.G,
                    b = BlackedOut_color_icon_color.B
                };

                // 色情報を定義（FixedFrame_color_iconの色を取得）
                SolidColorBrush FixedFrame_color_icon_brush = (SolidColorBrush)FixedFrame_color_icon.Foreground;
                Color FixedFrame_color_icon_color = FixedFrame_color_icon_brush.Color;
                ColorInfo FixedFrame_color_icon_color_Info = new ColorInfo
                {
                    r = FixedFrame_color_icon_color.R,
                    g = FixedFrame_color_icon_color.G,
                    b = FixedFrame_color_icon_color.B
                };
                FFMpeg_text.Text = "WoL Names detection process";

                string optimizationMode = ApplyOptimizationProfileToEnvironment();
                FFMpeg_text.Text = $"WoL Names detection process ({optimizationMode})";
                string effectiveCodec = GetEffectiveCodecForCurrentRun();
                if (ForX?.IsChecked == true)
                {
                    FFMpeg_text.Text = $"WoL Names detection process ({optimizationMode}, H.264 for X)";
                }

                if (cancel_state || isWindowClosing)
                {
                    stopwatch.Stop();
                    timer.Stop();
                    StopButton.IsEnabled = false;
                    FFMpeg_text.Text = "";
                    cancel_pending_state = false;
                    UIControl_enable_true();
                    running_state = false;
                    return;
                }

                StopButton.IsEnabled = false;
                stopwatch.Reset();
                stopwatch.Start();
                timer.Start();

                int processResult;

                {
                    processResult = await FrameProcessor.RunDmlMainAsync(video_temp_filename_1, video_temp_filename_2, effectiveCodec, hwaccel, v_width, v_height, v_fps, start_time, end_time, GetYoloThreshold(), v_color_primaries, rectInfos, rectInfos.Length, BlackedOut_color_icon_color_info, FixedFrame_color_icon_color_Info, Add_Copyright.IsChecked.Value, FrameProcessor.MaskTypeToNative(FrameProcessor.GetMaskTypeKind(GetComboText(BlackedOut_ComboBox, "Solid"))), FrameProcessor.MaskTypeToNative(FrameProcessor.GetMaskTypeKind(GetComboText(FixedFrame_ComboBox, "Solid"))), (int)BlackedOutSlideBar.Value, (int)FixedFrameSlideBar.Value, copyrightOffsetX, copyrightOffsetY, (float)copyrightZoomScale, copyrightImagePath ?? string.Empty, v_bitrate, preset, disableAudio, excludeByNameEnabled, ocrExpandPixels, ocrMaxRoisPerFrame, GetTextSimilarityThreshold(), maskExcludeTextCsv);
                }

                stopwatch.Stop();
                timer.Stop();
                StopButton.IsEnabled = false;

                if (isWindowClosing)
                {
                    cancel_pending_state = false;
                    return;
                }

                FFMpeg_text.Text = "";

                if (cancel_state == true)
                {
                    StopButton_icon.Glyph = "\uE71A";
                    InfoBar.Message = "Canceled";
                    InfoBar.Severity = InfoBarSeverity.Warning;
                    InfoBar.IsOpen = true;
                    InfoBar.Visibility = Visibility.Visible;
                }
                else if (processResult != 0)
                {
                    InfoBar.Message = $"Processing failed (code={processResult})";
                    InfoBar.Severity = InfoBarSeverity.Error;
                    InfoBar.IsOpen = true;
                    InfoBar.Visibility = Visibility.Visible;
                }
                else
                {
                    // Audio mux is already handled inside WoLNamesBlackedOut_DLL.
                    // Final copy remux normalizes container timestamps/chunk layout for better compatibility.
                    string final_output_video = video_temp_filename_2;
                    string normalizedOutput = System.IO.Path.Combine(tempDirectory, $"tmp_wol_{DateTime.Now:yyyyMMddHHmmssfff}_3.mp4");
                    string remuxArgs = disableAudio
                        ? $"-i \"{video_temp_filename_2}\" -map 0:v:0 -an -c copy -fflags +genpts -reset_timestamps 1 -avoid_negative_ts make_zero -movflags +faststart -f mp4 \"{normalizedOutput}\" -y"
                        : $"-i \"{video_temp_filename_2}\" -map 0:v:0 -map 0:a? -c copy -fflags +genpts -reset_timestamps 1 -avoid_negative_ts make_zero -movflags +faststart -f mp4 \"{normalizedOutput}\" -y";

                    var remuxInfo = new ProcessStartInfo
                    {
                        FileName = ffmpegPath,
                        Arguments = remuxArgs,
                        RedirectStandardOutput = true,
                        RedirectStandardError = true,
                        UseShellExecute = false,
                        CreateNoWindow = true
                    };

                    var remuxProcess = new Process { StartInfo = remuxInfo };
                    var remuxError = new StringBuilder();
                    remuxProcess.ErrorDataReceived += (s, e) =>
                    {
                        if (!string.IsNullOrEmpty(e.Data))
                        {
                            remuxError.AppendLine(e.Data);
                        }
                    };

                    remuxProcess.Start();
                    remuxProcess.BeginErrorReadLine();
                    await remuxProcess.WaitForExitAsync();

                    if (remuxProcess.ExitCode == 0 && File.Exists(normalizedOutput))
                    {
                        final_output_video = normalizedOutput;
                    }
                    else if (remuxError.Length > 0)
                    {
                        Debug.WriteLine("ffmpeg final remux error: " + remuxError.ToString());
                    }
                    //
                    var picker = new FileSavePicker();
                    InitializeWithWindow.Initialize(picker, WindowNative.GetWindowHandle(this));
                    picker.SuggestedStartLocation = PickerLocationId.VideosLibrary;
                    picker.FileTypeChoices.Add("MP4", new List<string>() { ".mp4" });
                    picker.DefaultFileExtension = ".mp4";
                    picker.SuggestedFileName = $"wol_{DateTime.Now:yyyyMMddHHmmssfff}";

                    StorageFile file = await picker.PickSaveFileAsync();
                    if (file != null)
                    {
                        try
                        {
                            // last_preview_image のファイルパスを取得
                            var sourceFile = await StorageFile.GetFileFromPathAsync(final_output_video);

                            // コピー先のファイルストリームを開く
                            using (var sourceStream = await sourceFile.OpenAsync(FileAccessMode.Read))
                            using (var destinationStream = await file.OpenAsync(FileAccessMode.ReadWrite))
                            {
                                // ストリームの内容をコピー
                                await sourceStream.AsStreamForRead().CopyToAsync(destinationStream.AsStreamForWrite());
                            }
                            InfoBar.Message = "output video is saved";
                            InfoBar.Severity = InfoBarSeverity.Success;
                            InfoBar.IsOpen = true;
                            InfoBar.Visibility = Visibility.Visible;
                        }
                        catch (Exception ex)
                        {
                            InfoBar.Message = "Save failed";
                            InfoBar.Severity = InfoBarSeverity.Error;
                            InfoBar.IsOpen = true;
                            InfoBar.Visibility = Visibility.Visible;
                        }
                    }
                }
                bool shouldReopenPreview = !cancel_state &&
                    !isWindowClosing &&
                    !string.IsNullOrWhiteSpace(v_file_path) &&
                    v_file_path.EndsWith(".mp4", StringComparison.OrdinalIgnoreCase) &&
                    !previewSessionOpen;

                cancel_pending_state = false;
                cancel_state = false;
                UIControl_enable_true();
                running_state = false;

                if (shouldReopenPreview)
                {
                    _ = ReopenPreviewAfterProcessingAsync(v_file_path);
                }
            }
        }

        private async Task ReopenPreviewAfterProcessingAsync(string sourcePath)
        {
            try
            {
                if (isWindowClosing || string.IsNullOrWhiteSpace(sourcePath) || running_state)
                {
                    return;
                }

                bool reopened = await StartRealtimePreviewSessionAsync(sourcePath);
                if (reopened && !isWindowClosing)
                {
                    previewFrameIndex = GetTargetFrameIndexFromSlider();
                    await PreviewSingleFrameAsync(previewFrameIndex);
                }
            }
            catch (Exception ex)
            {
                Debug.WriteLine($"ReopenPreviewAfterProcessingAsync failed: {ex.Message}");
            }
        }

        private async void BlackedOutStartButton_Checked(object sender, RoutedEventArgs e)
        {
            if (suppressBlackedOutToggleEvent)
            {
                return;
            }

            if (cancel_pending_state)
            {
                suppressBlackedOutToggleEvent = true;
                BlackedOutStartButton.IsChecked = false;
                suppressBlackedOutToggleEvent = false;
                return;
            }

            try
            {
                SetBlackedOutButtonVisual(true);
                cancel_state = false;
                await StartBlackedOutAsync();
            }
            catch (Exception ex)
            {
                Debug.WriteLine($"BlackedOutStartButton_Checked failed: {ex.Message}");
                InfoBar.Message = "Processing failed";
                InfoBar.Severity = InfoBarSeverity.Error;
                InfoBar.IsOpen = true;
                InfoBar.Visibility = Visibility.Visible;
            }
            finally
            {
                cancel_pending_state = false;
                suppressBlackedOutToggleEvent = true;
                BlackedOutStartButton.IsChecked = false;
                suppressBlackedOutToggleEvent = false;
                SetBlackedOutButtonVisual(false);
            }
        }

        private void BlackedOutStartButton_Unchecked(object sender, RoutedEventArgs e)
        {
            if (suppressBlackedOutToggleEvent)
            {
                return;
            }

            if (running_state)
            {
                EnterCancelPendingState();
                SafeCancelFfmpegProcesses();
                return;
            }

            SetBlackedOutButtonVisual(false);
        }

        private async void DrawingCanvas_PointerPressed(object sender, PointerRoutedEventArgs e)
        {
            if (running_state == true)
            {
                return;
            }

            // Check if Ctrl+Left-Click for copyright dragging
            bool isCtrlPressed = (e.KeyModifiers & Windows.System.VirtualKeyModifiers.Control) != 0;
            bool isLeftButton = e.GetCurrentPoint(DrawingCanvas).Properties.IsLeftButtonPressed;

            bool canEditCopyright = previewSessionOpen ||
                                    (PickAFileOutputTextBlock.Text?.EndsWith(".mp4", StringComparison.OrdinalIgnoreCase) == true);

            if (isCtrlPressed && isLeftButton && canEditCopyright)
            {
                // Start copyright dragging
                copyrightDragging = true;
                var point = e.GetCurrentPoint(DrawingCanvas).Position;
                copyrightDragStartX = point.X;
                copyrightDragStartY = point.Y;
                e.Handled = true;
                Debug.WriteLine("Copyright drag started");
                return;
            }

            if (e.GetCurrentPoint(DrawingCanvas).Properties.IsRightButtonPressed)
            {
                // 右クリックで矩形座標をリセット
                savedRects.Clear();
                Debug.WriteLine("矩形座標がリセットされました。");

                // 既存の矩形を全て削除
                RemoveAllRectangles();
                currentRectangle = null;
                isDrawing = false;
                e.Handled = true;

                if (previewSessionOpen && !previewTickBusy)
                {
                    previewFrameIndex = GetTargetFrameIndexFromSlider();
                    await PreviewSingleFrameAsync(previewFrameIndex);
                }
                else if (!previewSessionOpen && IsCurrentSourceImageFile())
                {
                    await RefreshCurrentImagePreviewAsync();
                }
                return;
            }
            else if (!isDrawing)
            {
                // 左クリックで矩形描画の開始
                startPoint = e.GetCurrentPoint(DrawingCanvas).Position;
                isDrawing = true;

                // ComboBoxの選択値を確認
                string value = GetComboText(FixedFrame_ComboBox, "Solid");

                if (FixedFrame_color != null && FixedFrameSlideBar != null)
                {
                    if (value == "Solid")
                    {
                        // Solidモード: プレビュー中は半透明の塗り
                        SolidColorBrush baseBrush = (SolidColorBrush)FixedFrame_color_icon.Foreground;
                        Color previewColor = baseBrush.Color;
                        previewColor.A = 128;  // 半透明（0～255、ここでは128）
                        SolidColorBrush previewBrush = new SolidColorBrush(previewColor);

                        currentRectangle = new Rectangle
                        {
                            Stroke = baseBrush,      // ストロークは元の色で
                            Fill = previewBrush,       // 一時的に半透明のFill
                            StrokeThickness = 2
                        };
                    }
                    else
                    {
                        // その他のモード: 赤い枠のみ、Fillは透明
                        currentRectangle = new Rectangle
                        {
                            Stroke = new SolidColorBrush(Colors.Red),
                            Fill = new SolidColorBrush(Colors.Transparent), // または null
                            StrokeThickness = 2,
                            StrokeDashArray = new DoubleCollection() { 4, 2 }  // 点線スタイルの例
                        };
                    }
                }
                else
                {
                    // プロパティが取得できない場合のデフォルト処理
                    currentRectangle = new Rectangle
                    {
                        Stroke = FixedFrame_color_icon.Foreground,
                        Fill = FixedFrame_color_icon.Foreground,
                        StrokeThickness = 2
                    };
                }

                Canvas.SetLeft(currentRectangle, startPoint.X);
                Canvas.SetTop(currentRectangle, startPoint.Y);
                DrawingCanvas.Children.Add(currentRectangle);
            }
            else
            {
                // 左クリックで矩形描画の完了
                isDrawing = false;

                if (currentRectangle == null)
                {
                    return;
                }

                // 2点クリック仕様: 2回目クリック位置を終点として矩形を確定
                var endPoint = e.GetCurrentPoint(DrawingCanvas).Position;
                var finalWidth = Math.Abs(endPoint.X - startPoint.X);
                var finalHeight = Math.Abs(endPoint.Y - startPoint.Y);
                currentRectangle.Width = finalWidth;
                currentRectangle.Height = finalHeight;
                Canvas.SetLeft(currentRectangle, Math.Min(startPoint.X, endPoint.X));
                Canvas.SetTop(currentRectangle, Math.Min(startPoint.Y, endPoint.Y));

                // 描画完了時に、Solidモードなら不透明なFillに更新
                string value = GetComboText(FixedFrame_ComboBox, "Solid");
                if (value == "Solid")
                {
                    // プレビュー用半透明から、完全な塗り（不透明）に変更
                    currentRectangle.Fill = FixedFrame_color_icon.Foreground;
                    currentRectangle.Stroke = FixedFrame_color_icon.Foreground;
                }

                // 矩形の座標を保存
                var x = Canvas.GetLeft(currentRectangle);
                var y = Canvas.GetTop(currentRectangle);
                var width = currentRectangle.Width;
                var height = currentRectangle.Height;
                Debug.WriteLine($"現在の矩形の座標: 左上({x}, {y}), 幅: {width}, 高さ: {height}");

                double safeScaleFactor = scaleFactor;
                if (safeScaleFactor <= 0 || double.IsNaN(safeScaleFactor) || double.IsInfinity(safeScaleFactor))
                {
                    safeScaleFactor = 1.0;
                }

                x = Math.Round(x / safeScaleFactor);
                y = Math.Round(y / safeScaleFactor);
                width = Math.Round(width / safeScaleFactor);
                height = Math.Round(height / safeScaleFactor);

                savedRects.Add(new Windows.Foundation.Rect(x, y, width, height));

                Debug.WriteLine($"変換後の矩形の座標: 左上({x}, {y}), 幅: {width}, 高さ: {height}");

                currentRectangle = null;
                RemoveAllRectangles();
                //SaveImageButton.IsEnabled = false;

                if (previewSessionOpen && !previewTickBusy)
                {
                    previewFrameIndex = GetTargetFrameIndexFromSlider();
                    await PreviewSingleFrameAsync(previewFrameIndex);
                }
                else if (!previewSessionOpen)
                {
                    string fixedMaskType = GetComboText(FixedFrame_ComboBox, "Solid");
                    if (IsCurrentSourceImageFile() && !string.Equals(fixedMaskType, "Solid", StringComparison.Ordinal))
                    {
                        await RefreshCurrentImagePreviewAsync();
                    }
                    else
                    {
                        RedrawRectanglesWithNewColor();
                    }
                }
            }

        }

        private void DrawingCanvas_PointerMoved(object sender, PointerRoutedEventArgs e)
        {
            if (running_state == true)
            {
                return;
            }

            var currentPoint = e.GetCurrentPoint(DrawingCanvas).Position;

            // Handle copyright dragging (Ctrl+drag)
            if (copyrightDragging)
            {
                double deltaX = currentPoint.X - copyrightDragStartX;
                double deltaY = currentPoint.Y - copyrightDragStartY;

                var (scaleX, scaleY) = GetPreviewDisplayScales();

                // Update offsets (pixel-based movement)
                copyrightOffsetX += (int)Math.Round(deltaX / scaleX);
                copyrightOffsetY += (int)Math.Round(deltaY / scaleY);

                // Update drag start position for next move
                copyrightDragStartX = currentPoint.X;
                copyrightDragStartY = currentPoint.Y;

                // Send position update to native DLL if preview is open
                if (previewSessionOpen)
                {
                    FrameProcessor.SetCopyrightOffset(copyrightOffsetX, copyrightOffsetY);
                    _ = RefreshCurrentPreviewFrameIfPausedAsync();
                }

                Debug.WriteLine($"Copyright position: x={copyrightOffsetX}, y={copyrightOffsetY}");
                e.Handled = true;
                return;
            }

            if (!isDrawing || currentRectangle == null)
            {
                return;
            }

            // 矩形のサイズを更新
            var width = Math.Abs(currentPoint.X - startPoint.X);
            var height = Math.Abs(currentPoint.Y - startPoint.Y);

            currentRectangle.Width = width;
            currentRectangle.Height = height;

            // 矩形の位置を更新
            Canvas.SetLeft(currentRectangle, Math.Min(startPoint.X, currentPoint.X));
            Canvas.SetTop(currentRectangle, Math.Min(startPoint.Y, currentPoint.Y));

            // ComboBox の選択値をチェック
            string value = GetComboText(FixedFrame_ComboBox, "Solid");

            if (value == "Solid")
            {
                // Solidモード: 描画中は半透明の塗りでプレビュー
                SolidColorBrush baseBrush = (SolidColorBrush)FixedFrame_color_icon.Foreground;
                Color previewColor = baseBrush.Color;
                previewColor.A = 128; // 半透明（アルファ値 0～255 で調整）
                SolidColorBrush previewBrush = new SolidColorBrush(previewColor);

                currentRectangle.Stroke = baseBrush;       // ストロークは元の色で
                currentRectangle.Fill = previewBrush;        // Fill は半透明で
                currentRectangle.StrokeDashArray = null;       // 実線
            }
            else
            {
                // その他: 赤い点線の枠かつ内部は透明
                currentRectangle.Stroke = new SolidColorBrush(Colors.Red);
                currentRectangle.Fill = new SolidColorBrush(Colors.Transparent);
                currentRectangle.StrokeDashArray = new DoubleCollection() { 4, 2 };  // 点線スタイルの例
            }

        }

        // 矩形のみ削除するためのメソッド
        private void RemoveAllRectangles()
        {
            var rectangles = DrawingCanvas.Children.OfType<Rectangle>().ToList();
            foreach (var rectangle in rectangles)
            {
                DrawingCanvas.Children.Remove(rectangle);
            }
        }

        private void DrawingCanvas_PointerReleased(object sender, PointerRoutedEventArgs e)
        {
            // End copyright dragging
            if (copyrightDragging)
            {
                copyrightDragging = false;
                // Note: Position is already updated in PointerMoved
            }
        }

        private void DrawingCanvas_PointerWheelChanged(object sender, PointerRoutedEventArgs e)
        {
            bool isCtrlPressed = (e.KeyModifiers & Windows.System.VirtualKeyModifiers.Control) != 0;
            if (!isCtrlPressed)
            {
                return;
            }

            bool canEditCopyright = previewSessionOpen ||
                                    (PickAFileOutputTextBlock.Text?.EndsWith(".mp4", StringComparison.OrdinalIgnoreCase) == true);
            if (!canEditCopyright)
            {
                return;
            }

            // Zoom copyright image with mouse wheel
            var properties = e.GetCurrentPoint(DrawingCanvas).Properties;
            int wheelDelta = properties.MouseWheelDelta;

            if (wheelDelta != 0)
            {
                // Update zoom scale (positive delta = zoom in, negative = zoom out)
                double zoomFactor = wheelDelta > 0 ? 1.05 : 0.95;
                copyrightZoomScale *= zoomFactor;

                // Clamp zoom scale between 0.5 and 3.0
                copyrightZoomScale = Math.Max(0.5, Math.Min(3.0, copyrightZoomScale));

                // Notify native DLL about zoom change (if needed)
                Debug.WriteLine($"Copyright zoom scale: {copyrightZoomScale:F2}");

                if (previewSessionOpen)
                {
                    _ = RefreshCurrentPreviewFrameIfPausedAsync();
                }

                e.Handled = true;
            }
        }

        private async Task RefreshCurrentPreviewFrameIfPausedAsync()
        {
            if (isWindowClosing || !previewSessionOpen || previewTimer.IsEnabled || previewTickBusy)
            {
                return;
            }

            previewFrameIndex = GetTargetFrameIndexFromSlider();
            await PreviewSingleFrameAsync(previewFrameIndex, true);
        }

        private void PreviewMaskSetting_ValueChanged(object sender, RangeBaseValueChangedEventArgs e)
        {
            _ = RefreshCurrentPreviewFrameIfPausedAsync();
        }

        private void Add_Copyright_CheckedChanged(object sender, RoutedEventArgs e)
        {
            _ = RefreshCurrentPreviewFrameIfPausedAsync();
        }

        private void ExcludeByNameEnabledCheckBox_CheckedChanged(object sender, RoutedEventArgs e)
        {
            excludeByNameEnabled = ExcludeByNameEnabledCheckBox?.IsChecked == true;
            if (OpenExcludeByNameSettingsButton != null)
            {
                OpenExcludeByNameSettingsButton.IsEnabled = excludeByNameEnabled;
            }
            _ = RefreshCurrentPreviewFrameIfPausedAsync();
        }

        private float GetTextSimilarityThreshold()
        {
            return Math.Clamp(textSimilarityPercent, 50, 100) / 100.0f;
        }

        private async void OpenExcludeByNameSettingsButton_Click(object sender, RoutedEventArgs e)
        {
            var expandSlider = new Slider
            {
                Header = IsJapaneseUiCulture() ? "OCR用拡張ピクセル数" : "OCR Expand Pixels",
                Minimum = 0,
                Maximum = 5,
                StepFrequency = 1,
                SnapsTo = SliderSnapsTo.StepValues,
                Value = ocrExpandPixels,
                Margin = new Thickness(0, 0, 0, 8)
            };

            var maxRoiSlider = new Slider
            {
                Header = IsJapaneseUiCulture() ? "フレームごとのOCR最大ROI数" : "Max OCR ROIs per Frame",
                Minimum = 1,
                Maximum = 32,
                StepFrequency = 1,
                SnapsTo = SliderSnapsTo.StepValues,
                Value = ocrMaxRoisPerFrame,
                Margin = new Thickness(0, 0, 0, 8)
            };

            var similaritySlider = new Slider
            {
                Header = IsJapaneseUiCulture() ? "テキスト類似度(%)" : "Text Similarity (%)",
                Minimum = 50,
                Maximum = 100,
                StepFrequency = 1,
                SnapsTo = SliderSnapsTo.StepValues,
                Value = textSimilarityPercent,
                Margin = new Thickness(0, 0, 0, 8)
            };

            var excludeTextBox = new TextBox
            {
                Header = IsJapaneseUiCulture() ? "対象外テキスト(カンマ区切り)" : "Exclude Texts (comma-separated)",
                Text = maskExcludeTextCsv ?? string.Empty,
                TextWrapping = TextWrapping.Wrap,
                AcceptsReturn = true,
                MinHeight = 80
            };

            var panel = new StackPanel
            {
                Spacing = 6,
                Children =
                {
                    expandSlider,
                    maxRoiSlider,
                    similaritySlider,
                    excludeTextBox
                }
            };

            var dialog = new ContentDialog
            {
                Title = IsJapaneseUiCulture() ? "名前除外設定" : "Exclude by Name Settings",
                Content = panel,
                PrimaryButtonText = "OK",
                CloseButtonText = IsJapaneseUiCulture() ? "キャンセル" : "Cancel",
                DefaultButton = ContentDialogButton.Primary,
                XamlRoot = this.Content.XamlRoot
            };

            var result = await dialog.ShowAsync();
            if (result == ContentDialogResult.Primary)
            {
                ocrExpandPixels = (int)Math.Round(expandSlider.Value);
                ocrMaxRoisPerFrame = (int)Math.Round(maxRoiSlider.Value);
                textSimilarityPercent = (int)Math.Round(similaritySlider.Value);
                maskExcludeTextCsv = excludeTextBox.Text?.Trim() ?? string.Empty;
                _ = RefreshCurrentPreviewFrameIfPausedAsync();
            }
        }

        private void SetSliderToStartButton_Click(object sender, RoutedEventArgs e)
        {
            int currentSeconds = Math.Max(0, (int)Math.Floor(FrameSlideBar.Value));
            Start_min.Value = Math.Min(Start_min.Maximum, currentSeconds / 60);
            Start_sec.Value = currentSeconds % 60;
        }

        private void SetSliderToEndButton_Click(object sender, RoutedEventArgs e)
        {
            int currentSeconds = Math.Max(0, (int)Math.Floor(FrameSlideBar.Value));
            End_min.Value = Math.Min(End_min.Maximum, currentSeconds / 60);
            End_sec.Value = currentSeconds % 60;
        }

        private (double scaleX, double scaleY) GetPreviewDisplayScales()
        {
            double fallback = scaleFactor;
            if (fallback <= 0 || double.IsNaN(fallback) || double.IsInfinity(fallback))
            {
                fallback = 1.0;
            }

            double sx = fallback;
            double sy = fallback;

            if (lastPreviewFrameWidth > 0 && lastPreviewFrameHeight > 0)
            {
                double actualWidth = image_preview.ActualWidth > 0 ? image_preview.ActualWidth : image_preview.Width;
                double actualHeight = image_preview.ActualHeight > 0 ? image_preview.ActualHeight : image_preview.Height;

                if (actualWidth > 0)
                {
                    sx = actualWidth / lastPreviewFrameWidth;
                }
                if (actualHeight > 0)
                {
                    sy = actualHeight / lastPreviewFrameHeight;
                }
            }

            if (sx <= 0 || double.IsNaN(sx) || double.IsInfinity(sx)) sx = fallback;
            if (sy <= 0 || double.IsNaN(sy) || double.IsInfinity(sy)) sy = fallback;
            return (sx, sy);
        }

        private void MainWindow_KeyDown(object sender, KeyRoutedEventArgs e)
        {
            if (e.Key != Windows.System.VirtualKey.Escape)
            {
                return;
            }

            if (!isDrawing)
            {
                return;
            }

            isDrawing = false;
            if (currentRectangle != null)
            {
                DrawingCanvas.Children.Remove(currentRectangle);
                currentRectangle = null;
            }

            e.Handled = true;
            Debug.WriteLine("Fixed frame drawing canceled by ESC");
        }

        private void StopButton_Click(object sender, Microsoft.UI.Xaml.RoutedEventArgs e)
        {
            SafeCancelFfmpegProcesses();
            cancel_state = true;
            StopButton_icon.Glyph = "\uE916";

        }

        // 色が変更された時に矩形を再描画するメソッド
        private void RedrawRectanglesWithNewColor()
        {
            if (previewSessionOpen)
            {
                RemoveAllRectangles();
                return;
            }

            // FixedFrame_color_icon の色を取得
            SolidColorBrush newColor = (SolidColorBrush)FixedFrame_color_icon.Foreground;

            // ComboBoxの選択値を取得
            string value = GetComboText(FixedFrame_ComboBox, "Solid");

            if (IsCurrentSourceImageFile() && !string.Equals(value, "Solid", StringComparison.Ordinal))
            {
                _ = RefreshCurrentImagePreviewAsync();
                return;
            }

            // 既存の矩形を全て削除
            RemoveAllRectangles();

            // 保存された矩形の座標から再描画
            foreach (var rect in savedRects)
            {
                var scaledRect = new Windows.Foundation.Rect(
                    rect.X * scaleFactor,
                    rect.Y * scaleFactor,
                    rect.Width * scaleFactor,
                    rect.Height * scaleFactor);

                var rectangle = new Rectangle
                {
                    StrokeThickness = 2,
                    Width = scaledRect.Width,
                    Height = scaledRect.Height
                };
                // 範囲に応じたスタイルを適用
                if (value == "Solid")
                {
                    // 塗りつぶしあり
                    rectangle.Stroke = newColor;
                    rectangle.Fill = newColor;
                }
                else
                {
                    // 赤い縁取りのみ。Fillは透明に設定
                    rectangle.Stroke = new SolidColorBrush(Colors.Red);
                    rectangle.Fill = null;
                    rectangle.StrokeDashArray = new DoubleCollection() { 4, 2 };  // 点線スタイルの例
                }

                Canvas.SetLeft(rectangle, scaledRect.X);
                Canvas.SetTop(rectangle, scaledRect.Y);
                DrawingCanvas.Children.Add(rectangle);
            }
        }
        private void FixedFrame_color_icon_ColorChanged()
        {
            RedrawRectanglesWithNewColor();
            _ = RefreshCurrentPreviewFrameIfPausedAsync();
        }
        private void BlackedOut_ComboBox_SelectionChanged(object sender, SelectionChangedEventArgs e)
        {
            string value = GetComboText(BlackedOut_ComboBox, "Solid");
            if (BlackedOut_color != null && BlackedOutSlideBar != null)
            {
                ConfigureMaskSliderForType(BlackedOutSlideBar, value);
                if (value == "Solid")
                {
                    BlackedOut_color.Visibility = Visibility.Visible;
                    BlackedOutSlideBar.Visibility = Visibility.Collapsed;
                }
                else if (IsSliderMaskType(value))
                {
                    BlackedOut_color.Visibility = Visibility.Collapsed;
                    BlackedOutSlideBar.Visibility = Visibility.Visible;
                }
                else
                {
                    BlackedOut_color.Visibility = Visibility.Collapsed;
                    BlackedOutSlideBar.Visibility = Visibility.Collapsed;
                }
            }

            _ = RefreshCurrentPreviewFrameIfPausedAsync();
        }

        private void FixedFrame_ComboBox_SelectionChanged(object sender, SelectionChangedEventArgs e)
        {
            string value = GetComboText(FixedFrame_ComboBox, "Solid");
            if (FixedFrame_color != null && FixedFrameSlideBar != null)
            {
                ConfigureMaskSliderForType(FixedFrameSlideBar, value);
                if (value == "Solid")
                {
                    FixedFrame_color.Visibility = Visibility.Visible;
                    FixedFrameSlideBar.Visibility = Visibility.Collapsed;
                }
                else if (IsSliderMaskType(value))
                {
                    FixedFrame_color.Visibility = Visibility.Collapsed;
                    FixedFrameSlideBar.Visibility = Visibility.Visible;
                }
                else
                {
                    FixedFrame_color.Visibility = Visibility.Collapsed;
                    FixedFrameSlideBar.Visibility = Visibility.Collapsed;
                }
                RedrawRectanglesWithNewColor();
            }

            _ = RefreshCurrentPreviewFrameIfPausedAsync();
        }

        private void Start_End_min_sec_ValueChanged(NumberBox sender, NumberBoxValueChangedEventArgs args)
        {
            if (Start_min == null || Start_sec == null || End_min == null || End_sec == null || ForX == null)
            {
                return;
            }
            if (End_min.Value * 60 + End_sec.Value > Start_min.Value * 60 + Start_sec.Value)
            { 
                BlackedOutStartButton.IsEnabled = true;
            }
            else
            {
                BlackedOutStartButton.IsEnabled = false;
            }
            if (End_min.Value * 60 + End_sec.Value - (Start_min.Value * 60 + Start_sec.Value) <= 140 && End_min.Value * 60 + End_sec.Value > Start_min.Value * 60 + Start_sec.Value)
            {
                ForX.IsEnabled = true;
            }
            else
            {
                ForX.IsEnabled = false;
                ForX.IsChecked = false;
            }

        }
    }
}
