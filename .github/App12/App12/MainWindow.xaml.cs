using Microsoft.UI;
using Microsoft.UI.Windowing;
using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;
using Microsoft.UI.Xaml.Controls.Primitives;
using Microsoft.UI.Xaml.Input;
using Microsoft.UI.Xaml.Media;
using Microsoft.UI.Xaml.Media.Imaging;
using Microsoft.UI.Xaml.Shapes;
using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.Globalization;
using System.IO;
using System.Linq;
using System.Runtime.InteropServices;
using System.Runtime.InteropServices.WindowsRuntime;
using System.Text;
using System.Text.RegularExpressions;
using System.Threading;
using System.Threading.Tasks;
using Windows.ApplicationModel;
using Windows.ApplicationModel.DataTransfer;
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
        private string ffmpegPath = System.IO.Path.Combine(AppContext.BaseDirectory, "ffmpeg-master-latest-win64-lgpl", "bin", "ffmpeg.exe");
        private Windows.Foundation.Point startPoint;
        private Rectangle currentRectangle;
        private bool isDrawing = false; // 矩形描画中かどうかを示すフラグ
        private List<Windows.Foundation.Rect> savedRects = []; // 保存された矩形の座標を保持するためのフィールド
        private string last_preview_image;
        private double scaleFactor;
        private string codec;
        private string hwaccel;
        private string preset;
        private bool trt_mode = false;
        private bool running_state = false; // 実行中かどうかを示すフラグ
        private bool cancel_state = false; // キャンセルかどうかを示すフラグ
        private bool v_hasAudio = true; // 音声があるかどうかを示すフラグ


        private Microsoft.UI.Xaml.DispatcherTimer timer;
        private Stopwatch stopwatch;

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


        [DllImport("WoLNamesBlackedOut_Util.dll", CallingConvention = CallingConvention.StdCall)]
        private static extern int GetCUDAComputeCapability();
        [DllImport("WoLNamesBlackedOut_Util.dll", CallingConvention = CallingConvention.StdCall)]
        private static extern char GetGpuVendor();
        [DllImport("WoLNamesBlackedOut_Util.dll", CallingConvention = CallingConvention.StdCall)]
        private static extern int onnx2trt();
        [DllImport("WoLNamesBlackedOut_Util.dll", CallingConvention = CallingConvention.StdCall)]
        private static extern bool GetRTXisEnable();
        [DllImport("WoLNamesBlackedOut_yolo.dll", CallingConvention = CallingConvention.StdCall)]
        static extern int get_total_frame_count();
        [DllImport("WoLNamesBlackedOut_yolo.dll", CallingConvention = CallingConvention.StdCall)]
        static extern bool CancelFfmpegProcesses();
        public MainWindow()
        {
            this.InitializeComponent();
            ExtendsContentIntoTitleBar = true;
            this.SetTitleBar(TitleBar);
            // Set the preferred height option for the title bar
            this.AppWindow.TitleBar.PreferredHeightOption = TitleBarHeightOption.Collapsed;

            try
            {
                // DPIスケールを取得
                double dpiScale = WindowHelper.GetWindowDpiScale(this);

                // 論理サイズをDPIスケールで変換
                int logicalWidth = 1900;
                int logicalHeight = 800;
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

            DrawingCanvas.PointerPressed += DrawingCanvas_PointerPressed;
            DrawingCanvas.PointerMoved += DrawingCanvas_PointerMoved;
            DrawingCanvas.PointerReleased += DrawingCanvas_PointerReleased;

            UIControl_enable_false();
            PickAFileButton.IsEnabled = true;


            // LocalSettings から値を読み込む
            var localSettings = Windows.Storage.ApplicationData.Current.LocalSettings;

            // Add_Copyright の設定を読み込み
            if (localSettings.Values.TryGetValue("Add_Copyright", out object addCopyrightValue))
            {
                if (bool.TryParse(addCopyrightValue.ToString(), out bool isChecked))
                {
                    Add_Copyright.IsChecked = isChecked;
                }
            }

            // BitrateSlideBar の値を読み込み、反映
            if (localSettings.Values.TryGetValue("Bitrate", out object bitrateValue))
            {
                if (double.TryParse(bitrateValue.ToString(), out double sliderValue))
                {
                    BitrateSlideBar.Value = sliderValue;
                }
            }

            // BitrateSlideBar の値を読み込み、反映
            if (localSettings.Values.TryGetValue("AppTheme", out object themeValue))
            {
                if (themeValue.ToString() == "Dark")
                {
                    RootGrid.RequestedTheme = ElementTheme.Dark;
                }
                else
                {
                    RootGrid.RequestedTheme = ElementTheme.Light;
                }
            }

            // AppDataのパスを取得
            StorageFolder localFolder = ApplicationData.Current.LocalFolder;
            string localAppDataPath = localFolder.Path;

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

            string dllDirectory = System.IO.Path.Combine(AppDomain.CurrentDomain.BaseDirectory);
            Environment.CurrentDirectory = dllDirectory;
            // 現在の実行ディレクトリを取得
            string currentDirectory = Environment.CurrentDirectory;

            // デバッグ情報としてコンソールに表示
            Console.WriteLine($"現在の実行ディレクトリ: {currentDirectory}");
            if (File.Exists("WoLNamesBlackedOut_Util.dll"))
            {
                Console.WriteLine("DLLファイルが存在します");
            }
            else
            {
                Console.WriteLine("DLLファイルが存在しません");
            }

            char gpuvendor = GetGpuVendor();
            if (gpuvendor == 'N')   //NVIDIAの場合
            {
                codec = "hevc_nvenc";
                hwaccel = "cuda";
                preset = "slow"; // NVIDIAのプリセットを設定

                if ((GetCUDAComputeCapability() > 75) || (GetCUDAComputeCapability() == 75 && GetRTXisEnable() == true))    //tensorRTが使える
                {
                    trt_mode = true;
                    ConvertButton.IsEnabled = true;

                    // アプリケーション専用のフォルダパスを取得
                    string filepath = System.IO.Path.Combine(appFolder, "my_yolov8m_s_20251004.engine");
                    if (!File.Exists(filepath))
                    {
                        // appFolder 内の "my_yolov8m*.engine" にマッチするファイル一覧を取得する
                        string[] engineFiles = Directory.GetFiles(appFolder, "my_yolov8m*.*");
                        foreach (string file in engineFiles)
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
                        ConvertButton_Click(null, null);
                    }
                    //Use_TensorRT.IsEnabled = true;
                    Use_TensorRT.IsChecked = true;
                }
            }
            else if (gpuvendor == 'A')  //AMDの場合
            {
                codec = "hevc_amf";
                hwaccel = "d3d11va";
                preset = "quality"; // AMDのプリセットを設定（必要に応じて変更可能）
                trt_mode = false;
                Use_TensorRT.IsEnabled = false;
                Use_TensorRT.IsChecked = false;
                ConvertButton.IsEnabled = false;
            }
            else if (gpuvendor == 'I')　//Intelの場合
            {
                codec = "hevc_qsv";
                hwaccel = "qsv";
                preset = "slow"; // Intelのプリセットを設定（必要に応じて変更可能）
                trt_mode = false;
                Use_TensorRT.IsEnabled = false;
                Use_TensorRT.IsChecked = false;
                ConvertButton.IsEnabled = false;
            }
            else //その他のベンダーの場合(gpuvendor == 'X')
            {
                UIControl_enable_false();
                InfoBar.Message = "Sorry, we could not find any available hardware encoders. The app cannot run.";
                InfoBar.Severity = InfoBarSeverity.Error;
                InfoBar.IsOpen = true;

                InfoBar.Visibility = Visibility.Visible;
            }

            if (gpuvendor != 'X')
            {
                PickAFileButton.IsEnabled = true;
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
            this.Close();
        }

        private void Window_Closed(object sender, WindowEventArgs e)
        {
            //ウィンドウが閉じた際のイベント
            var localSettings = Windows.Storage.ApplicationData.Current.LocalSettings;

            // チェックボックスの状態を保存する
            localSettings.Values["Add_Copyright"] = Add_Copyright.IsChecked;
            //localSettings.Values["Use_TensorRT"] = Use_TensorRT.IsChecked;
            localSettings.Values["Bitrate"] = BitrateSlideBar.Value;
            localSettings.Values["AppTheme"] = RootGrid.ActualTheme == ElementTheme.Light ? "Light" : "Dark";
            localSettings.Values["DeviceName"] = "";

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
                RootGrid.RequestedTheme = ElementTheme.Dark;
            }
            else
            {
                RootGrid.RequestedTheme = ElementTheme.Light;
            }
        }

        private void ToggleThemeButton_Click(object sender, RoutedEventArgs e)
        {
            ToggleTheme();
        }

        private void Timer_Tick(object? sender, object e)
        {

            // 経過時間を秒単位で表示
            double elapsedSeconds = stopwatch.Elapsed.TotalSeconds;
            Elapsed.Text = elapsedSeconds.ToString("F1"); // 小数点以下1桁まで表示
            int frame_count = get_total_frame_count();
            if (frame_count > 0)
            {
                double fps = ((double)frame_count / elapsedSeconds);
                double percentage = ((double)frame_count / (v_fps * (v_end_time - v_start_time)));
                double eta = (elapsedSeconds * (1 - percentage) / percentage + 0.5);
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

            PickAFileOutputTextBlock.Text = "";

            if (file == null)
            {
                PickAFileOutputTextBlock.Text = "Operation cancelled.";
                ProgressBar.IsIndeterminate = false;
                UIControl_enable_true();
                running_state = false;
                return;
            }

            ProgressBar.IsIndeterminate = true;
            var fileExtension = file.FileType.ToLower();
            PickAFileOutputTextBlock.Text = file.Name;
            v_file_path = file.Path;

            if (fileExtension == ".jpg" || fileExtension == ".jpeg" || fileExtension == ".png")
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

                await FrameProcessor.Runpreview_apiAsync(outputPath, rectInfos, rectInfos.Length, BlackedOut_color_icon_color_info, FixedFrame_color_icon_color_Info, Add_Copyright.IsChecked.Value, (string)BlackedOut_ComboBox.SelectedValue, (string)FixedFrame_ComboBox.SelectedValue, (int)BlackedOutSlideBar.Value, (int)FixedFrameSlideBar.Value);

                last_preview_image = outputPath;
                using (var stream = await file.OpenStreamForReadAsync())
                {
                    var bitmapImage = new BitmapImage(new Uri(outputPath));
                    bitmapImage.ImageOpened += (s, ev) =>
                    {
                        var originalHeight = bitmapImage.PixelHeight;
                        scaleFactor = image_preview.Height / originalHeight;
                        image_preview.Source = bitmapImage;
                        ProgressBar.IsIndeterminate = false;
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
                var properties = await GetVideoProperties(file);
                if (properties != null)
                {
                    if (int.TryParse(properties["width"], out int width))
                        v_width = width;
                    if (int.TryParse(properties["height"], out int height))
                        v_height = height;
                    if (double.TryParse(properties["r_frame_rate"], out double frameRate))
                        v_fps = (int)frameRate;
                    if (int.TryParse(properties["nb_frames"], out int nbFrames))
                    {
                        v_nb_frames = nbFrames;
                        float mod_frame = (float)(nbFrames % frameRate);
                        int mod_frame_sec = mod_frame > 0 ? 1 : 0;
                        int all_sec = (int)(nbFrames / frameRate);
                        int all_min = (int)(all_sec / 60);
                        int mod_sec = (int)(all_sec - all_min * 60) + mod_frame_sec;
                        if (mod_sec == 60)
                        {
                            all_min = all_min + 1;
                            mod_sec = 0;
                        }
                        Start_min.Value = 0;
                        Start_sec.Value = 0;
                        End_min.Value = all_min;
                        End_sec.Value = mod_sec;
                        Start_min.Maximum = all_min;
                        End_min.Maximum = all_min;
                        FrameSlideBar.Value = 0;
                        FrameSlideBar.Maximum = all_sec - mod_frame_sec;
                        FrameTextBlock_e.Text = $"{all_min}:{mod_sec.ToString("D2")}";
                    }
                    v_color_primaries = properties["color_primaries"];
                    v_hasAudio = properties.TryGetValue("has_audio", out var audioValue) && audioValue == "true";
                }
            }

            ProgressBar.IsIndeterminate = false;
            UIControl_enable_true();
            running_state = false;
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

        public class FrameProcessor
        {
            [DllImport("WoLNamesBlackedOut_yolo.dll", CallingConvention = CallingConvention.StdCall)]
            private static extern int dml_main(
                [MarshalAs(UnmanagedType.LPStr)] string input_video_path,
                [MarshalAs(UnmanagedType.LPStr)] string output_video_path,
                [MarshalAs(UnmanagedType.LPStr)] string codec,
                [MarshalAs(UnmanagedType.LPStr)] string hwaccel,
                int width, int height, int fps,
                [MarshalAs(UnmanagedType.LPStr)] string color_primaries,
                [In] RectInfo[] rects, int count,
                ColorInfo name_color, ColorInfo fixframe_color,
                //bool inpaint, bool copyright, bool no_inference);
                bool copyright,
                [MarshalAs(UnmanagedType.LPStr)] string blackedOut,
                [MarshalAs(UnmanagedType.LPStr)] string fixedFrame,
                int blackedout_param,
                int fixedFrame_param,
                [MarshalAs(UnmanagedType.LPStr)] string bitrate,
                [MarshalAs(UnmanagedType.LPStr)] string preset
                );

            [DllImport("WoLNamesBlackedOut_yolo.dll", CallingConvention = CallingConvention.StdCall)]
            private static extern int trt_main(
                [MarshalAs(UnmanagedType.LPStr)] string input_video_path,
                [MarshalAs(UnmanagedType.LPStr)] string output_video_path,
                [MarshalAs(UnmanagedType.LPStr)] string codec,
                [MarshalAs(UnmanagedType.LPStr)] string hwaccel,
                int width, int height, int fps,
                [MarshalAs(UnmanagedType.LPStr)] string color_primaries,
                [In] RectInfo[] rects, int count,
                ColorInfo name_color, ColorInfo fixframe_color,
                //bool inpaint, bool copyright, bool no_inference);
                bool copyright,
                [MarshalAs(UnmanagedType.LPStr)] string blackedOut,
                [MarshalAs(UnmanagedType.LPStr)] string fixedFrame,
                int blackedout_param,
                int fixedFrame_param,
                [MarshalAs(UnmanagedType.LPStr)] string bitrate,
                [MarshalAs(UnmanagedType.LPStr)] string preset
                );

            [DllImport("WoLNamesBlackedOut_yolo.dll", CallingConvention = CallingConvention.StdCall)]
            private static extern int preview_api(
                [MarshalAs(UnmanagedType.LPStr)] string image_path_str,
                [In] RectInfo[] rects,
                int count,
                ColorInfo name_color,
                ColorInfo fixframe_color,
                //bool inpaint,
                bool copyright,
                //bool no_inference);
                [MarshalAs(UnmanagedType.LPStr)] string blackedOut,
                [MarshalAs(UnmanagedType.LPStr)] string fixedFrame,
                int blackedout_param,
                int fixedFrame_param
                );

            public static Task<int> RunDmlMainAsync(
                string inputVideoPath, string outputVideoPath, string codec, string hwaccel,
                int width, int height, int fps, string colorPrimaries, RectInfo[] rects,
                int count, ColorInfo nameColor, ColorInfo fixframeColor,
                bool copyright, string blackedOut, string fixedFrame, int blackedout_param, int fixedFrame_param, string bitrate, string preset)
            {
                return Task.Run(() => dml_main(inputVideoPath, outputVideoPath, codec, hwaccel, width, height, fps, colorPrimaries, rects, count, nameColor, fixframeColor, copyright, blackedOut, fixedFrame, blackedout_param, fixedFrame_param, bitrate, preset));
            }

            public static Task<int> RunTrtMainAsync(
                string inputVideoPath, string outputVideoPath, string codec, string hwaccel,
                int width, int height, int fps, string colorPrimaries, RectInfo[] rects,
                int count, ColorInfo nameColor, ColorInfo fixframeColor,
                 bool copyright, string blackedOut, string fixedFrame, int blackedout_param, int fixedFrame_param, string bitrate, string preset)
            {
                return Task.Run(() => trt_main(inputVideoPath, outputVideoPath, codec, hwaccel, width, height, fps, colorPrimaries, rects, count, nameColor, fixframeColor, copyright, blackedOut, fixedFrame, blackedout_param, fixedFrame_param, bitrate, preset));
            }
            public static Task<int> Runpreview_apiAsync(
                string image_path_str,
                RectInfo[] rects,
                int count, ColorInfo nameColor, ColorInfo fixframeColor,
                bool copyright, string blackedOut, string fixedFrame, int blackedout_param, int fixedFrame_param)
            {
                return Task.Run(() => preview_api(image_path_str, rects, count, nameColor, fixframeColor, copyright, blackedOut, fixedFrame, blackedout_param, fixedFrame_param));
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
                    // last_preview_image のファイルパスを取得
                    var sourceFile = await StorageFile.GetFileFromPathAsync(last_preview_image);

                    // コピー先のファイルストリームを開く
                    using (var sourceStream = await sourceFile.OpenAsync(FileAccessMode.Read))
                    using (var destinationStream = await file.OpenAsync(FileAccessMode.ReadWrite))
                    {
                        // ストリームの内容をコピー
                        await sourceStream.AsStreamForRead().CopyToAsync(destinationStream.AsStreamForWrite());
                    }
                    //await new MessageDialog("画像が正常に保存されました。").ShowAsync();
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
                        SaveImageButton.IsEnabled = false;
                    }
                }
            }

        }
        private async Task<Dictionary<string, string>> GetVideoProperties(StorageFile file)
        {
            var ffprobePath = System.IO.Path.Combine(AppContext.BaseDirectory, "ffmpeg-master-latest-win64-lgpl", "bin", "ffprobe.exe");
            var arguments = $"-v error -select_streams v:0 -show_entries stream=width,height,r_frame_rate,nb_frames,color_primaries -of default=nw=1:nk=1 \"{file.Path}\"";

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
            var keys = new[] { "width", "height", "color_primaries", "r_frame_rate", "nb_frames" };

            for (int i = 0; i < lines.Length && i < keys.Length; i++)
            {
                if (keys[i] == "r_frame_rate")
                {
                    var parts = lines[i].Split('/');
                    if (parts.Length == 2 && int.TryParse(parts[0], out int numerator) && int.TryParse(parts[1], out int denominator))
                    {
                        properties[keys[i]] = (numerator / (double)denominator).ToString();
                    }
                }
                else
                {
                    properties[keys[i]] = lines[i];
                }

            }

            return properties;
        }
        private void UIControl_enable_false()
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
            ConvertButton.IsEnabled = false;
            Add_Copyright.IsEnabled = false;
            Use_TensorRT.IsEnabled = false;
            BlackedOut_ComboBox.IsEnabled = false;
            FixedFrame_ComboBox.IsEnabled = false;
            BlackedOutSlideBar.IsEnabled = false;
            FixedFrameSlideBar.IsEnabled = false;
            BitrateSlideBar.IsEnabled = false;

        }
        private void UIControl_enable_true()
        {
            PickAFileButton.IsEnabled = true;
            if ((PickAFileOutputTextBlock.Text != "Operation cancelled.") && ((PickAFileOutputTextBlock.Text != "")))
            {
                PreviewButton.IsEnabled = true;
                SaveImageButton.IsEnabled = true;
                string fileName = PickAFileOutputTextBlock.Text;
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
                SaveImageButton.IsEnabled = false;
                BlackedOutStartButton.IsEnabled = false;
            }
            BlackedOut_color.IsEnabled = true;
            FixedFrame_color.IsEnabled = true;
            FrameSlideBar.IsEnabled = true;
            Start_min.IsEnabled = true;
            Start_sec.IsEnabled = true;
            End_min.IsEnabled = true;
            End_sec.IsEnabled = true;
            Add_Copyright.IsEnabled = true;
            BlackedOut_ComboBox.IsEnabled = true;
            FixedFrame_ComboBox.IsEnabled = true;
            BlackedOutSlideBar.IsEnabled = true;
            FixedFrameSlideBar.IsEnabled = true;
            BitrateSlideBar.IsEnabled = true;
            if (trt_mode == false)
            {
                Use_TensorRT.IsEnabled = false;
                Use_TensorRT.IsChecked = false;
                ConvertButton.IsEnabled = false;
            }
            else
            {
                Use_TensorRT.IsEnabled = true;
                ConvertButton.IsEnabled = true;
            }
        }

        // スライダーの値が変更されたときに呼ばれるイベントハンドラ
        private void FrameSlideBar_ValueChanged(object sender, RangeBaseValueChangedEventArgs e)
        {
            // 現在のスライダーの値を取得

            int currentFrame = (int)e.NewValue;
            float n_mod_frame = (float)(currentFrame % v_fps);
            int n_mod_frame_sec = 1;
            if (n_mod_frame > 0)
            {
                n_mod_frame_sec = 1;
            }
            else
            {
                n_mod_frame_sec = 0;
            }
            int n_min = (int)(currentFrame / 60);
            int n_mod_sec = (int)(currentFrame - n_min * 60) + n_mod_frame_sec;
            if (n_mod_sec == 60)
            {
                n_min = n_min + 1;
                n_mod_sec = 0;
            }
            FrameTextBlock_n.Text = $"{n_min}:{n_mod_sec.ToString("D2")}";
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
                        new HyperlinkButton { Content = "Donate(buymeacoffee.com)", NavigateUri = new Uri("https://buymeacoffee.com/calocenrieti"), Margin = new Microsoft.UI.Xaml.Thickness(0, 0, 0, 12) ,HorizontalAlignment = HorizontalAlignment.Center},
                        new TextBlock { Text = "This software contains source code provided by NVIDIA Corporation.", FontSize=12,Margin = new Microsoft.UI.Xaml.Thickness(0, 0, 0, 8) ,HorizontalAlignment = HorizontalAlignment.Center ,TextWrapping=TextWrapping.Wrap},
                        new TextBlock { Text = "This software uses FFmpeg.exe licensed under the LGPLv2.1 \nand its source can be downloaded https://github.com/FFmpeg/FFmpeg.git", FontSize=12,Margin = new Microsoft.UI.Xaml.Thickness(0, 0, 0, 8) ,HorizontalAlignment = HorizontalAlignment.Center,TextWrapping=TextWrapping.Wrap}
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
                            new HyperlinkButton { Content = "Microsoft.AI.DirectML 1.15.4", NavigateUri = new Uri("https://www.nuget.org/packages/Microsoft.AI.DirectML/1.15.4/license"), Margin = new Microsoft.UI.Xaml.Thickness(0, 0, 0, 10) ,HorizontalAlignment = HorizontalAlignment.Center },
                            new HyperlinkButton { Content = "Microsoft.ML.OnnxRuntime 1.23.2", NavigateUri = new Uri("https://www.nuget.org/packages/Microsoft.ML.OnnxRuntime/1.23.2/license"), Margin = new Microsoft.UI.Xaml.Thickness(0, 0, 0, 10) ,HorizontalAlignment = HorizontalAlignment.Center },
                            new HyperlinkButton { Content = "Microsoft.ML.OnnxRuntime.DirectML 1.23.0", NavigateUri = new Uri("https://www.nuget.org/packages/Microsoft.ML.OnnxRuntime.DirectML/1.23.0/license"), Margin = new Microsoft.UI.Xaml.Thickness(0, 0, 0, 10) ,HorizontalAlignment = HorizontalAlignment.Center },
                            new HyperlinkButton { Content = "Microsoft.Windows.CppWinRT 2.0.250303.1", NavigateUri = new Uri("https://www.nuget.org/packages/Microsoft.Windows.CppWinRT/2.0.250303.1/license"), Margin = new Microsoft.UI.Xaml.Thickness(0, 0, 0, 10) ,HorizontalAlignment = HorizontalAlignment.Center },
                            new HyperlinkButton { Content = "Microsoft.NET.ILLink.Tasks 8.0.14", NavigateUri = new Uri("https://licenses.nuget.org/MIT"), Margin = new Microsoft.UI.Xaml.Thickness(0, 0, 0, 10) ,HorizontalAlignment = HorizontalAlignment.Center },
                            new HyperlinkButton { Content = "Microsoft.Windows.SDK.BuildTools 10.0.26100.4654", NavigateUri = new Uri("https://aka.ms/WinSDKLicenseURL"), Margin = new Microsoft.UI.Xaml.Thickness(0, 0, 0, 10) ,HorizontalAlignment = HorizontalAlignment.Center },
                            new HyperlinkButton { Content = "Microsoft.WindowsAppSDK 1.7.250606001", NavigateUri = new Uri("https://www.nuget.org/packages/Microsoft.WindowsAppSDK/1.7.250606001/license"), Margin = new Microsoft.UI.Xaml.Thickness(0, 0, 0, 10) ,HorizontalAlignment = HorizontalAlignment.Center },
                            new HyperlinkButton { Content = "NVIDIA CUDA Toolkit 12.9", NavigateUri = new Uri("https://docs.nvidia.com/cuda/eula/index.html"), Margin = new Microsoft.UI.Xaml.Thickness(0, 0, 0, 10) ,HorizontalAlignment = HorizontalAlignment.Center },
                            new HyperlinkButton { Content = "NVIDIA TensorRT-RTX 1.1", NavigateUri = new Uri("https://docs.nvidia.com/deeplearning/tensorrt-rtx/latest/_static/eula-12Aug2025.pdf"), Margin = new Microsoft.UI.Xaml.Thickness(0, 0, 0, 10) ,HorizontalAlignment = HorizontalAlignment.Center },
                            new HyperlinkButton { Content = "OpenCV 4.13dev", NavigateUri = new Uri("https://github.com/opencv/opencv/blob/master/LICENSE"), Margin = new Microsoft.UI.Xaml.Thickness(0, 0, 0, 10) ,HorizontalAlignment = HorizontalAlignment.Center },
                            new HyperlinkButton { Content = "FFmpeg 8.0 (exe)", NavigateUri = new Uri("https://www.gnu.org/licenses/old-licenses/lgpl-2.1.html"), Margin = new Microsoft.UI.Xaml.Thickness(0, 0, 0, 10) ,HorizontalAlignment = HorizontalAlignment.Center },
                            new HyperlinkButton { Content = "Ultralytics 8.3.184 (model)", NavigateUri = new Uri("https://www.ultralytics.com/legal/agpl-3-0-software-license"), Margin = new Microsoft.UI.Xaml.Thickness(0, 0, 0, 10) ,HorizontalAlignment = HorizontalAlignment.Center },
                        }
                    }
                },
                CloseButtonText = "Close"
            };

            aboutDialog.XamlRoot = this.Content.XamlRoot;
            await aboutDialog.ShowAsync();
        }

        private async void PreviewButton_Click(object sender, Microsoft.UI.Xaml.RoutedEventArgs e)
        {
            ProgressBar.IsIndeterminate = true;
            //PreviewButton.IsEnabled = false;
            UIControl_enable_false();
            running_state = true;

            // 現在の日時を使用してユニークなファイル名を作成し、一時ディレクトリに保存
            string tempDirectory = System.IO.Path.GetTempPath();
            string fileName = $"tmp_wol_{DateTime.Now:yyyyMMddHHmmssfff}.png";
            string outputPath = System.IO.Path.Combine(tempDirectory, fileName);
            string PickAFileOutputTextBlock_text = PickAFileOutputTextBlock.Text;

            if (PickAFileOutputTextBlock_text.EndsWith(".mp4", StringComparison.OrdinalIgnoreCase)) //動画の時
            {
                // 動画ファイルのパスを指定
                var videoFilePath = v_file_path; 

                // 現在のスライダーの値を取得
                int currentFrame = (int)FrameSlideBar.Value;

                // ffmpeg を使用して指定のフレームを抽出して PNG に保存
                await ExtractFrameToPng(videoFilePath, currentFrame, outputPath);
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

            await FrameProcessor.Runpreview_apiAsync(outputPath, rectInfos, rectInfos.Length, BlackedOut_color_icon_color_info, FixedFrame_color_icon_color_Info, Add_Copyright.IsChecked.Value, (string)BlackedOut_ComboBox.SelectedValue, (string)FixedFrame_ComboBox.SelectedValue, (int)BlackedOutSlideBar.Value, (int)FixedFrameSlideBar.Value);

            // 保存された PNG ファイルを Image コントロールに表示
            var bitmapImage = new BitmapImage(new Uri(outputPath));

            bitmapImage.ImageOpened += (s, ev) =>
            {
                var originalHeight = bitmapImage.PixelHeight;
                scaleFactor = image_preview.Height / originalHeight;
                // 縮小率を表示
                Debug.WriteLine($"縮小率: {scaleFactor}");

                image_preview.Source = bitmapImage;
                ProgressBar.IsIndeterminate = false;
                //PreviewButton.IsEnabled = true;
                UIControl_enable_true();
                running_state = false;

                // 描画されている矩形を全て消去
                RemoveAllRectangles();

            };

            // BitmapImage の読み込みを開始
            image_preview.Source = bitmapImage;
            SaveImageButton.IsEnabled = true;
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

        private async void BlackedOutStartButton_Click(object sender, Microsoft.UI.Xaml.RoutedEventArgs e)
        {
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
                running_state = true;
                bool Trim_skip = true;
                string tempDirectory = System.IO.Path.GetTempPath();
                string video_temp_filename_1 = System.IO.Path.Combine(tempDirectory, $"tmp_wol_{DateTime.Now:yyyyMMddHHmmssfff}_1.mp4");
                string video_temp_filename_2 = System.IO.Path.Combine(tempDirectory, $"tmp_wol_{DateTime.Now:yyyyMMddHHmmssfff}_2.mp4");
                string video_temp_filename_3 = System.IO.Path.Combine(tempDirectory, $"tmp_wol_{DateTime.Now:yyyyMMddHHmmssfff}_3.mp4");
                string audio_temp_filename = System.IO.Path.Combine(tempDirectory, $"tmp_wol_{DateTime.Now:yyyyMMddHHmmssfff}.aac");

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

                string v_bitrate = ((int)BitrateSlideBar.Value).ToString() + "M";

                if ((start_time != 0) || (end_time != all_sec))
                {
                    FFMpeg_text.Text = "Trimming video process";

                    Trim_skip = false;
                    int duration = end_time - start_time;
                    string arguments = $"-hwaccel \"{hwaccel}\" -ss \"{start_time}\" -t \"{duration}\" -i \"{v_file_path}\" -vcodec copy -acodec copy -f mp4 -b:v \"{v_bitrate}\" -preset \"{preset}\"  \"{video_temp_filename_1}\" -y";


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
                else
                {
                    video_temp_filename_1 = v_file_path;
                }
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

                StopButton.IsEnabled = true;
                stopwatch.Reset();
                stopwatch.Start();
                timer.Start();

                if (Use_TensorRT.IsChecked == true && (string)BlackedOut_ComboBox.SelectedValue != "No_Inference")
                {
                    // AppDataのパスを取得
                    StorageFolder localFolder = ApplicationData.Current.LocalFolder;
                    string localAppDataPath = localFolder.Path;
                    string appFolder = System.IO.Path.Combine(localAppDataPath, "WoLNamesBlackedOut");
                    string cashefilepath = System.IO.Path.Combine(appFolder, "my_yolov8m_s_20251004.cashe");
                    if (!File.Exists(cashefilepath))
                    {
                        InfoBar.Message = "JIT compile has started; processing will begin 10 seconds later.";
                        InfoBar.Severity = InfoBarSeverity.Success;
                        InfoBar.IsOpen = true;
                        InfoBar.Visibility = Visibility.Visible;
                    }
                    await FrameProcessor.RunTrtMainAsync(video_temp_filename_1, video_temp_filename_2, codec, hwaccel, v_width, v_height, v_fps, v_color_primaries, rectInfos, rectInfos.Length, BlackedOut_color_icon_color_info, FixedFrame_color_icon_color_Info, Add_Copyright.IsChecked.Value, (string)BlackedOut_ComboBox.SelectedValue, (string)FixedFrame_ComboBox.SelectedValue, (int)BlackedOutSlideBar.Value, (int)FixedFrameSlideBar.Value, v_bitrate, preset);
                }
                else
                {
                    await FrameProcessor.RunDmlMainAsync(video_temp_filename_1, video_temp_filename_2, codec, hwaccel, v_width, v_height, v_fps, v_color_primaries, rectInfos, rectInfos.Length, BlackedOut_color_icon_color_info, FixedFrame_color_icon_color_Info, Add_Copyright.IsChecked.Value, (string)BlackedOut_ComboBox.SelectedValue, (string)FixedFrame_ComboBox.SelectedValue, (int)BlackedOutSlideBar.Value, (int)FixedFrameSlideBar.Value, v_bitrate, preset);
                }

                stopwatch.Stop();
                timer.Stop();
                StopButton.IsEnabled = false;

                FFMpeg_text.Text = "";

                if (cancel_state == true)
                {
                    StopButton_icon.Glyph = "\uE71A";
                    InfoBar.Message = "Canceled";
                    InfoBar.Severity = InfoBarSeverity.Warning;
                    InfoBar.IsOpen = true;
                    InfoBar.Visibility = Visibility.Visible;
                }
                else
                {
                    if (v_hasAudio)
                    {
                        await movie_audio_process(audio_temp_filename, video_temp_filename_2, video_temp_filename_3, Trim_skip, v_bitrate);
                    }
                    else
                    {
                        video_temp_filename_3 = video_temp_filename_2;

                    }
                    //
                    var picker = new FileSavePicker();
                    InitializeWithWindow.Initialize(picker, WindowNative.GetWindowHandle(this));
                    picker.SuggestedStartLocation = PickerLocationId.VideosLibrary;
                    picker.FileTypeChoices.Add("MP4", new List<string>() { ".mp4" });
                    picker.DefaultFileExtension = ".mp4";
                    picker.SuggestedFileName = "output";

                    StorageFile file = await picker.PickSaveFileAsync();
                    if (file != null)
                    {
                        try
                        {
                            // last_preview_image のファイルパスを取得
                            var sourceFile = await StorageFile.GetFileFromPathAsync(video_temp_filename_3);

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
                cancel_state = false;
                UIControl_enable_true();
                running_state = false;
            }
        }

        private async Task movie_audio_process(string audioFile1, string videoFile2, string outvideo, bool Trim_skip, string v_bitrate)
        {
            FFMpeg_text.Text = "Audio process";
            //音声分離
            string arguments = "";
            if (Trim_skip == true) //トリムなしの時
            {
                arguments = $" -i \"{v_file_path}\" -vn -acodec copy \"{audioFile1}\"";
            }
            else //トリムありのとき
            {
                int start_time = (int)(Start_min.Value * 60) + (int)(Start_sec.Value);
                int end_time = (int)(End_min.Value * 60) + (int)(End_sec.Value);
                int duration = end_time - start_time;
                arguments = $" -i \"{v_file_path}\" -ss \"{start_time}\" -t \"{duration}\" -acodec copy \"{audioFile1}\"";
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

            //音声を映像と合成

            arguments = "";
            if (Trim_skip == true)
            {
                arguments = $" -i \"{audioFile1}\" -i \"{videoFile2}\" -c:v copy -c:a copy \"{outvideo}\"";
            }
            else
            {
                arguments = $"-hwaccel \"{hwaccel}\" -i \"{audioFile1}\" -i \"{videoFile2}\" -vcodec \"{codec}\" -b:v \"{v_bitrate}\" -preset \"{preset}\" \"{outvideo}\"";
            }

            processStartInfo = new ProcessStartInfo
            {
                FileName = ffmpegPath,
                Arguments = arguments,
                RedirectStandardOutput = true,
                RedirectStandardError = true,
                UseShellExecute = false,
                CreateNoWindow = true,
                StandardOutputEncoding = Encoding.UTF8, // UTF-8 エンコーディング
                StandardErrorEncoding = Encoding.UTF8    // UTF-8 エンコーディング
            };

            process = new Process { StartInfo = processStartInfo };
            output = new StringBuilder();
            error = new StringBuilder();

            process.OutputDataReceived += (sender, e) => AppendOutput(e.Data);
            process.ErrorDataReceived += (sender, e) => AppendOutput(e.Data);

            process.Start();
            process.BeginOutputReadLine();
            process.BeginErrorReadLine();
            await process.WaitForExitAsync();

            // エラーメッセージを表示
            if (error.Length > 0)
            {
                Debug.WriteLine("ffmpeg error: " + error.ToString());
            }

            FFMpeg_text.Text = "";

        }
        private void AppendOutput(string data)
        {
            if (!string.IsNullOrEmpty(data))
            {
                // ffmpeg の出力例を想定: "frame= 123 fps= 37 q=-1.0 Lsize=  12345kB time=00:00:05.12 bitrate=19769.5kbits/s speed=1.23x"
                // 下記の正規表現は、"frame="、"fps="、"time=" の数値を抽出する例です。
                Regex regex = new Regex(@"frame=\s*(\d+).*fps=\s*([\d\.]+).*time=\s*([0-9:\.]+)", RegexOptions.IgnoreCase);
                var match = regex.Match(data);
                if (match.Success)
                {
                    // 各数値をパース
                    if (int.TryParse(match.Groups[1].Value, out int frame_ffmpeg))
                    {
                        double.TryParse(match.Groups[2].Value, NumberStyles.Float, CultureInfo.InvariantCulture, out double fps_ffmpeg);
                        string timeStr = match.Groups[3].Value;  // 必要に応じて TimeSpan にパースできます
                        if (TimeSpan.TryParse(timeStr, out TimeSpan timeParsed))
                        {
                            // timeParsed.TotalSeconds は double 型で総秒数を返す
                            double totalSeconds = timeParsed.TotalSeconds;

                            // UI の進捗表示（例: フレーム数、FPS）を更新
                            DispatcherQueue.TryEnqueue(() =>
                            {
                                //double fps = fps_ffmpeg ;
                                double percentage = ((double)frame_ffmpeg / (v_fps * (v_end_time - v_start_time)));
                                double eta = (totalSeconds * (1 - percentage) / percentage + 0.5);
                                Elapsed.Text = totalSeconds.ToString("F1");
                                FPS.Text = fps_ffmpeg.ToString("F2");
                                ETA.Text = eta.ToString("F2");
                                ProgressBar.Value = percentage * 100;
                            });
                        }
                    }
                }
            }
        }
        private void DrawingCanvas_PointerPressed(object sender, PointerRoutedEventArgs e)
        {
            if (running_state == true)
            {
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
                SaveImageButton.IsEnabled = false;
            }
            else if (!isDrawing)
            {
                // 左クリックで矩形描画の開始
                startPoint = e.GetCurrentPoint(DrawingCanvas).Position;
                isDrawing = true;

                // ComboBoxの選択値を確認
                string value = (string)FixedFrame_ComboBox.SelectedValue;

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

                // 描画完了時に、Solidモードなら不透明なFillに更新
                string value = (string)FixedFrame_ComboBox.SelectedValue;
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

                x = Math.Round(x / scaleFactor);
                y = Math.Round(y / scaleFactor);
                width = Math.Round(width / scaleFactor);
                height = Math.Round(height / scaleFactor);

                savedRects.Add(new Windows.Foundation.Rect(x, y, width, height));

                Debug.WriteLine($"変換後の矩形の座標: 左上({x}, {y}), 幅: {width}, 高さ: {height}");

                currentRectangle = null;
                SaveImageButton.IsEnabled = false;
            }

        }

        private void DrawingCanvas_PointerMoved(object sender, PointerRoutedEventArgs e)
        {
            if (!isDrawing || currentRectangle == null || running_state == true)
            {
                return;
            }

            // 現在のマウスポインター位置を取得
            var currentPoint = e.GetCurrentPoint(DrawingCanvas).Position;

            // 矩形のサイズを更新
            var width = Math.Abs(currentPoint.X - startPoint.X);
            var height = Math.Abs(currentPoint.Y - startPoint.Y);

            currentRectangle.Width = width;
            currentRectangle.Height = height;

            // 矩形の位置を更新
            Canvas.SetLeft(currentRectangle, Math.Min(startPoint.X, currentPoint.X));
            Canvas.SetTop(currentRectangle, Math.Min(startPoint.Y, currentPoint.Y));

            // ComboBox の選択値をチェック
            string value = (string)FixedFrame_ComboBox.SelectedValue;

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
            // このメソッドでは特に何もしません

        }

        private async void ConvertButton_Click(object sender, Microsoft.UI.Xaml.RoutedEventArgs e)
        {
            // AppDataのパスを取得
            StorageFolder localFolder = ApplicationData.Current.LocalFolder;
            string localAppDataPath = localFolder.Path;
            string appFolder = System.IO.Path.Combine(localAppDataPath, "WoLNamesBlackedOut");
            // appFolder 内の "my_yolov8m*.engine" にマッチするファイル一覧を取得する
            string[] engineFiles = Directory.GetFiles(appFolder, "my_yolov8m*.*");
            foreach (string file in engineFiles)
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

            InfoBar.Severity = InfoBarSeverity.Informational;
            //InfoBar.Message = "Converting to TensorRT. Please wait about 360 seconds.";
            InfoBar.Message = "Converting to TensorRT-RTX. Please wait about 5 seconds.";
            InfoBar.Visibility = Visibility.Visible;

            UIControl_enable_false();
            running_state = true;
            ProgressBar.IsIndeterminate = true;
            stopwatch.Reset();
            stopwatch.Start();
            timer.Start();

            await Task.Run(() =>
            {
                Thread.CurrentThread.Priority = ThreadPriority.Highest; // スレッドの優先度を最高に設定
                ConvertOnnxFileToTensorRt();

            });

            stopwatch.Stop();
            timer.Stop();

            UIControl_enable_true();
            running_state = false;
            ProgressBar.IsIndeterminate = false;
            Console.WriteLine("変換成功");
            InfoBar.Severity = InfoBarSeverity.Success;
            InfoBar.Message = "TensorRT-RTX Engine build success!";
            InfoBar.Visibility = Visibility.Visible;
            Use_TensorRT.IsEnabled = true;
            Use_TensorRT.IsChecked = true;
        }

        private void ConvertOnnxFileToTensorRt()
        {

            int res = onnx2trt();

            // 返値に基づいて処理を分岐
            if (res == 0)
            {
                Console.WriteLine("変換成功");
            }
            else
            {
                Console.WriteLine("変換失敗");
            }

        }

        private void StopButton_Click(object sender, Microsoft.UI.Xaml.RoutedEventArgs e)
        {
            CancelFfmpegProcesses();
            cancel_state = true;
            StopButton_icon.Glyph = "\uE916";

        }

        // 色が変更された時に矩形を再描画するメソッド
        private void RedrawRectanglesWithNewColor()
        {
            // FixedFrame_color_icon の色を取得
            SolidColorBrush newColor = (SolidColorBrush)FixedFrame_color_icon.Foreground;

            // ComboBoxの選択値を取得
            string value = (string)FixedFrame_ComboBox.SelectedValue;

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
        }
        private void BlackedOut_ComboBox_SelectionChanged(object sender, SelectionChangedEventArgs e)
        {
            string value = (string)BlackedOut_ComboBox.SelectedValue;
            if (BlackedOut_color != null && BlackedOutSlideBar != null)
            {
                if (value == "Solid")
                {
                    BlackedOut_color.Visibility = Visibility.Visible;
                    BlackedOutSlideBar.Visibility = Visibility.Collapsed;
                }
                else if (value == "Mosaic" || value == "Blur")
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
        }

        private void FixedFrame_ComboBox_SelectionChanged(object sender, SelectionChangedEventArgs e)
        {
            string value = (string)FixedFrame_ComboBox.SelectedValue;
            if (FixedFrame_color != null && FixedFrameSlideBar != null)
            {
                if (value == "Solid")
                {
                    FixedFrame_color.Visibility = Visibility.Visible;
                    FixedFrameSlideBar.Visibility = Visibility.Collapsed;
                }
                else if (value == "Mosaic" || value == "Blur")
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
        }
    }

}
