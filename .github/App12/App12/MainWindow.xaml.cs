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
using Windows.UI.Input.Inking;
using WinRT.Interop;


// To learn more about WinUI, the WinUI project structure,
// and more about our project templates, see: http://aka.ms/winui-project-info.

namespace WoLNamesBlackedOut
{
    /// <summary>
    /// An empty window that can be used on its own or navigated to within a Frame.
    /// </summary>


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
        //private CancellationTokenSource cancellationTokenSource;

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


        [DllImport("WoLNamesBlackedOut_Util.dll", CallingConvention = CallingConvention.Cdecl)]
        private static extern int GetCUDAComputeCapability();
        [DllImport("WoLNamesBlackedOut_Util.dll", CallingConvention = CallingConvention.Cdecl)]
        private static extern char GetGpuVendor();
        [DllImport("WoLNamesBlackedOut_Util.dll", CallingConvention = CallingConvention.Cdecl)]
        private static extern int onnx2trt();
        [DllImport("WoLNamesBlackedOut_Util.dll", CallingConvention = CallingConvention.Cdecl)]
        private static extern bool GetRTXisEnable();

        [DllImport("WoLNamesBlackedOut_yolo.dll", CallingConvention = CallingConvention.Cdecl)]
        static extern int get_total_frame_count();
        [DllImport("WoLNamesBlackedOut_yolo.dll", CallingConvention = CallingConvention.Cdecl)]
        static extern bool CancelFfmpegProcesses();
        public MainWindow()
        {
            this.InitializeComponent();
            ExtendsContentIntoTitleBar = true;
            this.SetTitleBar(TitleBar);
            // Set the preferred height option for the title bar
            this.AppWindow.TitleBar.PreferredHeightOption = TitleBarHeightOption.Collapsed;

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

            // Use_TensorRT の設定を読み込み
            if (localSettings.Values.TryGetValue("Use_TensorRT", out object useTensorRTValue))
            {
                if (bool.TryParse(useTensorRTValue.ToString(), out bool isChecked))
                {
                    Use_TensorRT.IsChecked = isChecked;
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
                preset = "slow"; // NVIDIAのプリセットを設定（必要に応じて変更可能）

                if (GetCUDAComputeCapability() > 75)    //tensorRTが使える
                {
                    trt_mode = true;
                    ConvertButton.IsEnabled = true;

                    // アプリケーション専用のフォルダパスを取得
                    string filepath = System.IO.Path.Combine(appFolder, "my_yolov8m_s_20250525.engine");

                    if (File.Exists(filepath))
                    {
                        //Use_TensorRT.IsEnabled = true;
                    }
                    else
                    {
                        Use_TensorRT.IsEnabled = false;
                        Use_TensorRT.IsChecked = false;

                        // appFolder 内の "my_yolov8m*.engine" にマッチするファイル一覧を取得する
                        string[] engineFiles = Directory.GetFiles(appFolder, "my_yolov8m*.engine");
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

                    }
                }
                else if (GetCUDAComputeCapability() == 75 && GetRTXisEnable() == true)
                {
                    trt_mode = true;
                    ConvertButton.IsEnabled = true;
                    // アプリケーション専用のフォルダパスを取得
                    string filepath = System.IO.Path.Combine(appFolder, "my_yolov8m_s_20250525.engine");
                    if (File.Exists(filepath))
                    {
                        //Use_TensorRT.IsEnabled = true;
                    }
                    else
                    {
                        Use_TensorRT.IsEnabled = false;
                        Use_TensorRT.IsChecked = false;

                        // appFolder 内の "my_yolov8m*.engine" にマッチするファイル一覧を取得する
                        string[] engineFiles = Directory.GetFiles(appFolder, "my_yolov8m*.engine");
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
                    }
                }
                else　   //tensorRTが使えない  
                {
                    trt_mode = false;
                    Use_TensorRT.IsEnabled = false;
                    Use_TensorRT.IsChecked = false;
                    ConvertButton.IsEnabled = false;
                }


            }
            else if (gpuvendor == 'A')  //AMDの場合
            {
                codec = "hevc_amf";
                hwaccel = "d3d11va";
                preset = "quality"; // AMDのプリセットを設定（必要に応じて変更可能）
                trt_mode = false;
                Use_TensorRT.IsEnabled = false;
                Use_TensorRT.IsChecked= false;
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
                //// DispatcherTimerを利用して10秒後にアプリケーションを終了する
                //DispatcherTimer timer = new DispatcherTimer
                //{
                //    Interval = TimeSpan.FromSeconds(10) // 10秒のインターバルを設定
                //};

                //timer.Tick += (sender, e) =>
                //{
                //    timer.Stop();  // 複数回の実行を防ぐためにタイマーを停止
                //    Environment.Exit(0);  // アプリケーションを終了
                //};

                //timer.Start(); // タイマーを開始
            }

            if (gpuvendor != 'X')
            {
                PickAFileButton.IsEnabled = true;
            }


        }

        private void MinimizeButton_Click(object sender, RoutedEventArgs e)
        {
            if(this.AppWindow.Presenter is OverlappedPresenter presenter)
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
            localSettings.Values["Use_TensorRT"] = Use_TensorRT.IsChecked;
            localSettings.Values["Bitrate"] = BitrateSlideBar.Value;
            localSettings.Values["AppTheme"] = RootGrid.ActualTheme == ElementTheme.Light ? "Light" : "Dark";

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

            //UIControl_enable_false();
            //running_state = true;

            //// Clear previous returned file name, if it exists, between iterations of this scenario
            //PickAFileOutputTextBlock.Text = "";

            //// Create a file picker
            //var openPicker = new Windows.Storage.Pickers.FileOpenPicker();

            //// See the sample code below for how to make the window accessible from the App class.
            //var window = App.MainWindow;

            //// Retrieve the window handle (HWND) of the current WinUI 3 window.
            //var hWnd = WinRT.Interop.WindowNative.GetWindowHandle(window);

            //// Initialize the file picker with the window handle (HWND).
            //WinRT.Interop.InitializeWithWindow.Initialize(openPicker, hWnd);

            //// Set options for your file picker
            //openPicker.ViewMode = PickerViewMode.Thumbnail;
            //openPicker.SuggestedStartLocation = PickerLocationId.VideosLibrary;
            //openPicker.FileTypeFilter.Add(".jpg");
            //openPicker.FileTypeFilter.Add(".jpeg");
            //openPicker.FileTypeFilter.Add(".png");
            //openPicker.FileTypeFilter.Add(".mp4");

            //// Open the picker for the user to pick a file
            //var file = await openPicker.PickSingleFileAsync();
            //if (file == null)
            //{
            //    PickAFileOutputTextBlock.Text = "Operation cancelled.";
            //}
            //else
            //{
            //    ProgressBar.IsIndeterminate = true;
            //    var fileExtension = file.FileType.ToLower();
            //    PickAFileOutputTextBlock.Text = file.Name;
            //    v_file_path = file.Path;
            //    if (fileExtension == ".jpg" || fileExtension == ".jpeg" || fileExtension == ".png")
            //    {
            //        // 現在の日時を使用してユニークなファイル名を作成し、一時ディレクトリに保存
            //        string tempDirectory = System.IO.Path.GetTempPath();
            //        string fileName = $"tmp_wol_{DateTime.Now:yyyyMMddHHmmssfff}.png";
            //        string outputPath = System.IO.Path.Combine(tempDirectory, fileName);

            //        File.Copy(v_file_path, outputPath, true);
            //        File.Copy(v_file_path, outputPath, true);

            //        // RectInfo 配列を作成
            //        RectInfo[] rectInfos = savedRects.Select(rect => new RectInfo
            //        {
            //            x = (int)rect.X,
            //            y = (int)rect.Y,
            //            width = (int)rect.Width,
            //            height = (int)rect.Height
            //        }).ToArray();

            //        // 色情報を定義（BlackedOut_color_iconの色を取得）
            //        SolidColorBrush BlackedOut_color_icon_brush = (SolidColorBrush)BlackedOut_color_icon.Foreground;
            //        Color BlackedOut_color_icon_color = BlackedOut_color_icon_brush.Color;
            //        ColorInfo BlackedOut_color_icon_color_info = new ColorInfo
            //        {
            //            r = BlackedOut_color_icon_color.R,
            //            g = BlackedOut_color_icon_color.G,
            //            b = BlackedOut_color_icon_color.B
            //        };

            //        // 色情報を定義（FixedFrame_color_iconの色を取得）
            //        SolidColorBrush FixedFrame_color_icon_brush = (SolidColorBrush)FixedFrame_color_icon.Foreground;
            //        Color FixedFrame_color_icon_color = FixedFrame_color_icon_brush.Color;
            //        ColorInfo FixedFrame_color_icon_color_Info = new ColorInfo
            //        {
            //            r = FixedFrame_color_icon_color.R,
            //            g = FixedFrame_color_icon_color.G,
            //            b = FixedFrame_color_icon_color.B
            //        };

            //        //await FrameProcessor.Runpreview_apiAsync(outputPath, rectInfos, rectInfos.Length, BlackedOut_color_icon_color_info, FixedFrame_color_icon_color_Info, Inpaint.IsChecked.Value, Add_Copyright.IsChecked.Value, noInference: !No_Inference.IsChecked.Value);
            //        await FrameProcessor.Runpreview_apiAsync(outputPath, rectInfos, rectInfos.Length, BlackedOut_color_icon_color_info, FixedFrame_color_icon_color_Info, Add_Copyright.IsChecked.Value, (string)BlackedOut_ComboBox.SelectedValue, (string)FixedFrame_ComboBox.SelectedValue, (int)BlackedOutSlideBar.Value, (int)FixedFrameSlideBar.Value);

            //        last_preview_image = outputPath;
            //        using (var stream = await file.OpenStreamForReadAsync())
            //        {
            //            // 保存された PNG ファイルを Image コントロールに表示
            //            var bitmapImage = new BitmapImage(new Uri(outputPath));

            //            bitmapImage.ImageOpened += (s, ev) =>
            //            {
            //                var originalHeight = bitmapImage.PixelHeight;
            //                scaleFactor = image_preview.Height / originalHeight;
            //                // 縮小率を表示
            //                Debug.WriteLine($"縮小率: {scaleFactor}");

            //                image_preview.Source = bitmapImage;
            //                ProgressBar.IsIndeterminate = false;

            //                UIControl_enable_true();
            //                running_state = false;

            //                // 描画されている矩形を全て消去
            //                RemoveAllRectangles();

            //            };

            //            // BitmapImage の読み込みを開始
            //            image_preview.Source = bitmapImage;
            //            SaveImageButton.IsEnabled = true;


            //        }
            //    }
            //    else if (fileExtension == ".mp4")
            //    {
            //        var properties = await GetVideoProperties(file);
            //        if (properties != null)
            //        {
            //            if (int.TryParse(properties["width"], out int width))
            //                //Debug.WriteLine($"Width: {width}");
            //                v_width = width;
            //            if (int.TryParse(properties["height"], out int height))
            //                //Debug.WriteLine($"Height: {height}");
            //                v_height = height;
            //            if (double.TryParse(properties["r_frame_rate"], out double frameRate))
            //            {
            //                //Debug.WriteLine($"Frame Rate: {frameRate}");
            //                v_fps = (int)frameRate;
            //            }
            //            if (int.TryParse(properties["nb_frames"], out int nbFrames))
            //            {
            //                //Debug.WriteLine($"Number of Frames: {nbFrames}");
            //                v_nb_frames = nbFrames;

            //                float mod_frame = (float)(nbFrames % frameRate);
            //                int mod_frame_sec = 1;
            //                if (mod_frame > 0)
            //                {
            //                    mod_frame_sec = 1;
            //                }
            //                else
            //                {
            //                    mod_frame_sec = 0;
            //                }
            //                int all_sec = (int)(nbFrames / frameRate);
            //                int all_min = (int)(all_sec / 60);
            //                int mod_sec = (int)(all_sec - all_min * 60) + mod_frame_sec;
            //                if (mod_sec == 60)
            //                {
            //                    all_min = all_min + 1;
            //                    mod_sec = 0;
            //                }
            //                Start_min.Value = 0;
            //                Start_sec.Value = 0;
            //                End_min.Value = all_min;
            //                End_sec.Value = mod_sec;
            //                Start_min.Maximum = all_min;
            //                End_min.Maximum = all_min;

            //                FrameSlideBar.Value = 0;

            //                FrameSlideBar.Maximum = all_sec - mod_frame_sec;
            //                FrameTextBlock_e.Text = $"{all_min}:{mod_sec.ToString("D2")}";
            //            }
            //            //Debug.WriteLine($"Color Primaries: {properties["color_primaries"]}");
            //            v_color_primaries = properties["color_primaries"];
            //            v_hasAudio = properties.TryGetValue("has_audio", out var audioValue) && audioValue == "true";
            //        }

            //    }

            //}

            //ProgressBar.IsIndeterminate = false;
            //UIControl_enable_true();
            //running_state= false;
        }

        public class FrameProcessor
        {
            [DllImport("WoLNamesBlackedOut_yolo.dll", CallingConvention = CallingConvention.Cdecl)]
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

            [DllImport("WoLNamesBlackedOut_yolo.dll", CallingConvention = CallingConvention.Cdecl)]
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

            [DllImport("WoLNamesBlackedOut_yolo.dll", CallingConvention = CallingConvention.Cdecl)]
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
                //bool inpaint, bool copyright, bool noInference)
                bool copyright, string blackedOut, string fixedFrame, int blackedout_param, int fixedFrame_param, string bitrate, string preset)
            {
                //return Task.Run(() => dml_main(inputVideoPath, outputVideoPath, codec, hwaccel, width, height, fps, colorPrimaries, rects, count, nameColor, fixframeColor, inpaint, copyright, noInference));
                //return Task.Run(() => dml_main(inputVideoPath, outputVideoPath, codec, hwaccel, width, height, fps, colorPrimaries, rects, count, nameColor, fixframeColor, copyright, blackedOut,fixedFrame, blackedout_param, fixedFrame_param,bitrate));
                return Task.Run(() => dml_main(inputVideoPath, outputVideoPath, codec, hwaccel, width, height, fps, colorPrimaries, rects, count, nameColor, fixframeColor, copyright, blackedOut,fixedFrame, blackedout_param, fixedFrame_param,bitrate,preset));
            }

            public static Task<int> RunTrtMainAsync(
                string inputVideoPath, string outputVideoPath, string codec, string hwaccel,
                int width, int height, int fps, string colorPrimaries, RectInfo[] rects,
                int count, ColorInfo nameColor, ColorInfo fixframeColor,
                 //bool inpaint, bool copyright, bool noInference)
                 bool copyright, string blackedOut, string fixedFrame, int blackedout_param, int fixedFrame_param, string bitrate,string preset)
            {
                //return Task.Run(() => trt_main(inputVideoPath, outputVideoPath, codec, hwaccel, width, height, fps, colorPrimaries, rects, count, nameColor, fixframeColor, inpaint, copyright, noInference));
                //return Task.Run(() => trt_main(inputVideoPath, outputVideoPath, codec, hwaccel, width, height, fps, colorPrimaries, rects, count, nameColor, fixframeColor, copyright,blackedOut,fixedFrame, blackedout_param, fixedFrame_param,bitrate));
                return Task.Run(() => trt_main(inputVideoPath, outputVideoPath, codec, hwaccel, width, height, fps, colorPrimaries, rects, count, nameColor, fixframeColor, copyright, blackedOut, fixedFrame, blackedout_param, fixedFrame_param, bitrate, preset));
            }
            public static Task<int> Runpreview_apiAsync(
                string image_path_str,
                RectInfo[] rects,
                int count, ColorInfo nameColor, ColorInfo fixframeColor,
                //bool inpaint, bool copyright, bool noInference)
                bool copyright, string blackedOut, string fixedFrame,int blackedout_param,int fixedFrame_param)
            {
                //return Task.Run(() => preview_api(image_path_str, rects, count, nameColor, fixframeColor, inpaint, copyright, noInference));
                return Task.Run(() => preview_api(image_path_str, rects, count, nameColor, fixframeColor,  copyright, blackedOut, fixedFrame, blackedout_param, fixedFrame_param));
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
            //colorPicker.IsAlphaEnabled = true;
            //colorPicker.IsAlphaSliderVisible = true;
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
            //ReloadImageButton.IsEnabled = false;
            BlackedOutStartButton.IsEnabled = false;
            ConvertButton.IsEnabled = false;
            //Inpaint.IsEnabled = false;
            Add_Copyright.IsEnabled = false;
            Use_TensorRT.IsEnabled = false;
            //No_Inference.IsEnabled = false;
            BlackedOut_ComboBox.IsEnabled = false;
            FixedFrame_ComboBox.IsEnabled = false;
            BlackedOutSlideBar.IsEnabled = false;
            FixedFrameSlideBar.IsEnabled = false;
            BitrateSlideBar.IsEnabled = false;

        }
        private void UIControl_enable_true()
        {
            PickAFileButton.IsEnabled = true;
            if (PickAFileOutputTextBlock.Text != "Operation cancelled.")
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
            //Inpaint.IsEnabled = true;
            Add_Copyright.IsEnabled = true;
            //No_Inference.IsEnabled = true;
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
            StorageFolder localFolder = ApplicationData.Current.LocalFolder;
            string localAppDataPath = localFolder.Path;

            // アプリケーション専用のフォルダパスを取得
            string filepath = System.IO.Path.Combine(localAppDataPath, "WoLNamesBlackedOut", "my_yolov8m_s_20250525.engine");
            if (File.Exists(filepath))
            {
                Use_TensorRT.IsEnabled = true;
            }
            else
            {
                Use_TensorRT.IsEnabled = false;
                Use_TensorRT.IsChecked = false;
            }
        }

        // スライダーの値が変更されたときに呼ばれるイベントハンドラ
        private void FrameSlideBar_ValueChanged(object sender, RangeBaseValueChangedEventArgs e)
        {
            // 現在のスライダーの値を取得

            int currentFrame = (int)e.NewValue;
            //FrameTextBlock_n.Text = $"{currentFrame}";
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
                            new TextBlock { FontSize=16,Margin = new Microsoft.UI.Xaml.Thickness(0, 0, 0, 10),Text = "Microsoft.AI.DirectML 1.15.4" },
                            new TextBlock { FontSize=9,Margin = new Microsoft.UI.Xaml.Thickness(0, 0, 0, 20),TextWrapping=TextWrapping.Wrap ,Text = "﻿MICROSOFT SOFTWARE LICENSE TERMS\r\nMICROSOFT DIRECTX MACHINE LEARNING (DIRECTML)\r\n\r\nIF YOU LIVE IN (OR ARE A BUSINESS WITH A PRINCIPAL PLACE OF BUSINESS IN) THE UNITED STATES, PLEASE READ THE “BINDING ARBITRATION AND CLASS ACTION WAIVER” SECTION BELOW. IT AFFECTS HOW DISPUTES ARE RESOLVED.\r\n\r\nThese license terms are an agreement between you and Microsoft Corporation (or one of its affiliates). They apply to the software named above and any Microsoft services or software updates (except to the extent such services or updates are accompanied by new or additional terms, in which case those different terms apply prospectively and do not alter your or Microsoft’s rights relating to pre-updated software or services). IF YOU COMPLY WITH THESE LICENSE TERMS, YOU HAVE THE RIGHTS BELOW. BY USING THE SOFTWARE, YOU ACCEPT THESE TERMS.\r\n\r\n1. INSTALLATION AND USE RIGHTS.\r\n\ta) General. Subject to the terms of this agreement, you may install and use any number of copies of the software, and solely for use on Windows and Xbox. You may copy and distribute the software (i.e. make available for third parties) in applications and services you develop in the build with Machine Learning tools and frameworks, and/or games that run on Windows and Xbox.\r\n\tb) Included Microsoft Applications. The software may include other Microsoft applications. These license terms apply to those included applications, if any, unless other license terms are provided with the other Microsoft applications.\r\n\tc) Third Party Components. The software may include third party components with separate legal notices or governed by other agreements, as may be described in the ThirdPartyNotices file(s) accompanying the software.\r\n\r\n2. DATA. \r\n\ta) Data Collection. The software may collect information about you and your use of the software, and send that to Microsoft. Microsoft may use this information to provide services and improve our products and services. You may opt-out of many of these scenarios, but not all, as described in the product documentation.  There are also some features in the software that may enable you to collect data from users of your applications. If you use these features to enable data collection in your applications, you must comply with applicable law, including providing appropriate notices to users of your applications. You can learn more about data collection and use in the help documentation and the privacy statement at https://aka.ms/privacy. Your use of the software operates as your consent to these practices.\r\n\tb) Processing of Personal Data. To the extent Microsoft is a processor or subprocessor of personal data in connection with the software, Microsoft makes the commitments in the European Union General Data Protection Regulation Terms of the Online Services Terms to all customers effective May 25, 2018, at https://docs.microsoft.com/en-us/legal/gdpr.\r\n\r\n3. SCOPE OF LICENSE. The software is licensed, not sold. Microsoft reserves all other rights. Unless applicable law gives you more rights despite this limitation, you will not (and have no right to):\r\n\ta) work around any technical limitations in the software that only allow you to use it in certain ways;\r\n\tb) reverse engineer, decompile or disassemble the software, or otherwise attempt to derive the source code for the software, except and to the extent required by third party licensing terms governing use of certain open source components that may be included in the software;\r\n\tc) remove, minimize, block, or modify any notices of Microsoft or its suppliers in the software;\r\n\td) use the software in any way that is against the law or to create or propagate malware; or\r\n\te) except as expressly stated in Section 1, share, publish, distribute, or lease the software, provide the software as a stand-alone offering for others to use, or transfer the software or this agreement to any third party.\r\n\r\n4. EXPORT RESTRICTIONS. You must comply with all domestic and international export laws and regulations that apply to the software, which include restrictions on destinations, end users, and end use. For further information on export restrictions, visit https://aka.ms/exporting.\r\n\r\n5. SUPPORT SERVICES. Microsoft is not obligated under this agreement to provide any support services for the software. Any support provided is “as is”, “with all faults”, and without warranty of any kind. \r\n\r\n6. UPDATES. The software may periodically check for updates, and download and install them for you. You may obtain updates only from Microsoft or authorized sources. Microsoft may need to update your system to provide you with updates. You agree to receive these automatic updates without any additional notice. Updates may not include or support all existing software features, services, or peripheral devices.\r\n\r\n7. BINDING ARBITRATION AND CLASS ACTION WAIVER. This Section applies if you live in (or, if a business, your principal place of business is in) the United States.  If you and Microsoft have a dispute, you and Microsoft agree to try for 60 days to resolve it informally. If you and Microsoft can’t, you and Microsoft agree to binding individual arbitration before the American Arbitration Association under the Federal Arbitration Act (“FAA”), and not to sue in court in front of a judge or jury. Instead, a neutral arbitrator will decide. Class action lawsuits, class-wide arbitrations, private attorney-general actions, and any other proceeding where someone acts in a representative capacity are not allowed; nor is combining individual proceedings without the consent of all parties. The complete Arbitration Agreement contains more terms and is at https://aka.ms/arb-agreement-4. You and Microsoft agree to these terms.\r\n\r\n8. ENTIRE AGREEMENT. This agreement, and any other terms Microsoft may provide for supplements, updates, or third-party applications, is the entire agreement for the software.\r\n\r\n9. APPLICABLE LAW AND PLACE TO RESOLVE DISPUTES. If you acquired the software in the United States or Canada, the laws of the state or province where you live (or, if a business, where your principal place of business is located) govern the interpretation of this agreement, claims for its breach, and all other claims (including consumer protection, unfair competition, and tort claims), regardless of conflict of laws principles, except that the FAA governs everything related to arbitration. If you acquired the software in any other country, its laws apply, except that the FAA governs everything related to arbitration. If U.S. federal jurisdiction exists, you and Microsoft consent to exclusive jurisdiction and venue in the federal court in King County, Washington for all disputes heard in court (excluding arbitration). If not, you and Microsoft consent to exclusive jurisdiction and venue in the Superior Court of King County, Washington for all disputes heard in court (excluding arbitration).\r\n\r\n10. CONSUMER RIGHTS; REGIONAL VARIATIONS. This agreement describes certain legal rights. You may have other rights, including consumer rights, under the laws of your state, province, or country. Separate and apart from your relationship with Microsoft, you may also have rights with respect to the party from which you acquired the software. This agreement does not change those other rights if the laws of your state, province, or country do not permit it to do so. For example, if you acquired the software in one of the below regions, or mandatory country law applies, then the following provisions apply to you:\r\n\ta) Australia. You have statutory guarantees under the Australian Consumer Law and nothing in this agreement is intended to affect those rights.\r\n\tb) Canada. If you acquired this software in Canada, you may stop receiving updates by turning off the automatic update feature, disconnecting your device from the Internet (if and when you re-connect to the Internet, however, the software will resume checking for and installing updates), or uninstalling the software. The product documentation, if any, may also specify how to turn off updates for your specific device or software.\r\n\tc) Germany and Austria.\r\n\t\ti. Warranty. The properly licensed software will perform substantially as described in any Microsoft materials that accompany the software. However, Microsoft gives no contractual guarantee in relation to the licensed software.\r\n\t\tii. Limitation of Liability. In case of intentional conduct, gross negligence, claims based on the Product Liability Act, as well as, in case of death or personal or physical injury, Microsoft is liable according to the statutory law.\r\n\t\tSubject to the foregoing clause ii., Microsoft will only be liable for slight negligence if Microsoft is in breach of such material contractual obligations, the fulfillment of which facilitate the due performance of this agreement, the breach of which would endanger the purpose of this agreement and the compliance with which a party may constantly trust in (so-called \"cardinal obligations\"). In other cases of slight negligence, Microsoft will not be liable for slight negligence.\r\n\r\n11. DISCLAIMER OF WARRANTY. THE SOFTWARE IS LICENSED “AS IS.” YOU BEAR THE RISK OF USING IT. MICROSOFT GIVES NO EXPRESS WARRANTIES, GUARANTEES, OR CONDITIONS. TO THE EXTENT PERMITTED UNDER APPLICABLE LAWS, MICROSOFT EXCLUDES ALL IMPLIED WARRANTIES, INCLUDING MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE, AND NON-INFRINGEMENT.\r\n\r\n12. LIMITATION ON AND EXCLUSION OF DAMAGES. IF YOU HAVE ANY BASIS FOR RECOVERING DAMAGES DESPITE THE PRECEDING DISCLAIMER OF WARRANTY, YOU CAN RECOVER FROM MICROSOFT AND ITS SUPPLIERS ONLY DIRECT DAMAGES UP TO U.S. $5.00. YOU CANNOT RECOVER ANY OTHER DAMAGES, INCLUDING CONSEQUENTIAL, LOST PROFITS, SPECIAL, INDIRECT OR INCIDENTAL DAMAGES.\r\nThis limitation applies to (a) anything related to the software, services, content (including code) on third party Internet sites, or third party applications; and (b) claims for breach of contract, warranty, guarantee, or condition; strict liability, negligence, or other tort; or any other claim; in each case to the extent permitted by applicable law.\r\nIt also applies even if Microsoft knew or should have known about the possibility of the damages. The above limitation or exclusion may not apply to you because your state, province, or country may not allow the exclusion or limitation of incidental, consequential, or other damages."},
                            new TextBlock { FontSize=16,Margin = new Microsoft.UI.Xaml.Thickness(0, 0, 0, 10),Text = "Microsoft.ML.OnnxRuntime 1.22.1" },
                            new TextBlock { FontSize=9,Margin = new Microsoft.UI.Xaml.Thickness(0, 0, 0, 20),TextWrapping=TextWrapping.Wrap ,Text = "MIT License\r\n\r\nCopyright (c) Microsoft Corporation\r\n\r\nPermission is hereby granted, free of charge, to any person obtaining a copy\r\nof this software and associated documentation files (the \"Software\"), to deal\r\nin the Software without restriction, including without limitation the rights\r\nto use, copy, modify, merge, publish, distribute, sublicense, and/or sell\r\ncopies of the Software, and to permit persons to whom the Software is\r\nfurnished to do so, subject to the following conditions:\r\n\r\nThe above copyright notice and this permission notice shall be included in all\r\ncopies or substantial portions of the Software.\r\n\r\nTHE SOFTWARE IS PROVIDED \"AS IS\", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR\r\nIMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,\r\nFITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE\r\nAUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER\r\nLIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,\r\nOUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE\r\nSOFTWARE."},
                            new TextBlock { FontSize=16,Margin = new Microsoft.UI.Xaml.Thickness(0, 0, 0, 10),Text = "Microsoft.ML.OnnxRuntime.DirectML 1.22.1" },
                            new TextBlock { FontSize=9,Margin = new Microsoft.UI.Xaml.Thickness(0, 0, 0, 20),TextWrapping=TextWrapping.Wrap ,Text = "MIT License\r\n\r\nCopyright (c) Microsoft Corporation\r\n\r\nPermission is hereby granted, free of charge, to any person obtaining a copy\r\nof this software and associated documentation files (the \"Software\"), to deal\r\nin the Software without restriction, including without limitation the rights\r\nto use, copy, modify, merge, publish, distribute, sublicense, and/or sell\r\ncopies of the Software, and to permit persons to whom the Software is\r\nfurnished to do so, subject to the following conditions:\r\n\r\nThe above copyright notice and this permission notice shall be included in all\r\ncopies or substantial portions of the Software.\r\n\r\nTHE SOFTWARE IS PROVIDED \"AS IS\", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR\r\nIMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,\r\nFITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE\r\nAUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER\r\nLIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,\r\nOUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE\r\nSOFTWARE."},
                            new TextBlock { FontSize=16,Margin = new Microsoft.UI.Xaml.Thickness(0, 0, 0, 10),Text = "Microsoft.Windows.CppWinRT 2.0.250303.1" },
                            new TextBlock { FontSize=9,Margin = new Microsoft.UI.Xaml.Thickness(0, 0, 0, 20),TextWrapping=TextWrapping.Wrap ,Text = " MIT License\r\n\r\n    Copyright (c) Microsoft Corporation.\r\n\r\n    Permission is hereby granted, free of charge, to any person obtaining a copy\r\n    of this software and associated documentation files (the \"Software\"), to deal\r\n    in the Software without restriction, including without limitation the rights\r\n    to use, copy, modify, merge, publish, distribute, sublicense, and/or sell\r\n    copies of the Software, and to permit persons to whom the Software is\r\n    furnished to do so, subject to the following conditions:\r\n\r\n    The above copyright notice and this permission notice shall be included in all\r\n    copies or substantial portions of the Software.\r\n\r\n    THE SOFTWARE IS PROVIDED \"AS IS\", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR\r\n    IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,\r\n    FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE\r\n    AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER\r\n    LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,\r\n    OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE\r\n    SOFTWARE"},
                            new TextBlock { FontSize=16,Margin = new Microsoft.UI.Xaml.Thickness(0, 0, 0, 10),Text = "Microsoft.NET.ILLink.Tasks 8.0.14" },
                            new TextBlock { FontSize=9,Margin = new Microsoft.UI.Xaml.Thickness(0, 0, 0, 20),TextWrapping=TextWrapping.Wrap ,Text = "MIT License\r\nSPDX identifier\r\nMIT\r\nLicense text\r\nMIT License\r\n\r\nCopyright (c) <year> <copyright holders>\r\n\r\nPermission is hereby granted, free of charge, to any person obtaining a copy of this software and associated documentation files (the \"Software\"), to deal in the Software without restriction, including without limitation the rights to use, copy, modify, merge, publish, distribute, sublicense, and/or sell copies of the Software, and to permit persons to whom the Software is furnished to do so, subject to the following conditions:\r\n\r\nThe above copyright notice and this permission notice (including the next paragraph) shall be included in all copies or substantial portions of the Software.\r\n\r\nTHE SOFTWARE IS PROVIDED \"AS IS\", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.\r\n\r\nSPDX web page\r\nhttps://spdx.org/licenses/MIT.html\r\nNotice\r\nThis license content is provided by the SPDX project. For more information about licenses.nuget.org, see our documentation.\r\n\r\nData pulled from spdx/license-list-data on February 9, 2023."},
                            new TextBlock { FontSize=16,Margin = new Microsoft.UI.Xaml.Thickness(0, 0, 0, 10),Text = "Microsoft.Windows.SDK.BuildTools 10.0.26100.4654" },
                            new TextBlock { FontSize=9,Margin = new Microsoft.UI.Xaml.Thickness(0, 0, 0, 20),TextWrapping=TextWrapping.Wrap ,Text = "\r\nMICROSOFT SOFTWARE LICENSE TERMS\r\nMICROSOFT WINDOWS SOFTWARE DEVELOPMENT KIT (SDK) FOR WINDOWS 10 \r\n_______________________________________________________________________________________________________\r\nThese license terms are an agreement between Microsoft Corporation (or based on where you live, one of its affiliates) and you. Please read them. They apply to the software named above, which includes the media on which you received it, if any. The terms also apply to any Microsoft\r\n    • APIs (i.e., APIs included with the installation of the SDK or APIs accessed by installing extension packages or service to use with the SDK),\r\n    • updates,\r\n    • supplements,\r\n    • internet-based services, and\r\n    • support services\r\nfor this software, unless other terms accompany those items. If so, those terms apply.\r\nBy using the software, you accept these terms. If you do not accept them, do not use the software. \r\nAs described below, using some features also operates as your consent to the transmission of certain standard computer information for Internet-based services.\r\n________________________________________________________________________________________________ \r\nIf you comply with these license terms, you have the rights below.\r\n    1. INSTALLATION AND USE RIGHTS.  \r\n        a. You may install and use any number of copies of the software on your devices to design, develop and test your programs that run on a Microsoft operating system. Further, you may install, use and/or deploy via a network management system or as part of a desktop image, any number of copies of the software on computer devices within your internal corporate network to design, develop and test your programs that run on a Microsoft operating system. Each copy must be complete, including all copyright and trademark notices. You must require end users to agree to terms that protect the software as much as these license terms. \r\n        b. Utilities.  The software contains certain components that are identified in the Utilities List located at http://go.microsoft.com/fwlink/?LinkId=524839.  Depending on the specific edition of the software, the number of Utility files you receive with the software may not be equal to the number of Utilities listed in the Utilities List.   Except as otherwise provided on the Utilities List for specific files, you may copy and install the Utilities you receive with the software on to other third party machines. These Utilities may only be used to debug and deploy your programs and databases you have developed with the software.  You must delete all the Utilities installed onto a third party machine within the earlier of (i) when you have finished debugging or deploying your programs; or (ii) thirty (30) days after installation of the Utilities onto that machine. We may add additional files to this list from time to time.\r\n        c. Build Services and Enterprise Build Servers.  You may install and use any number of copies of the software onto your build machines or servers, solely for the purpose of:\r\n            i. Compiling, building, verifying and archiving your programs;\r\n            ii. Creating and configuring build systems internal to your organization to support your internal build environment; or\r\n            iii. Enabling a service for third parties to design, develop and test programs or services that run on a Microsoft operating system. \r\n        d. Included Microsoft Programs. The software contains other Microsoft programs. The license terms with those programs apply to your use of them.\r\n        e. Third Party Notices.  The software may include third party code that Microsoft, not the third party, licenses to you under this agreement. Notices, if any, for the third party code are included for your information only.  Notices, if any, for this third party code are included with the software and may be located at http://aka.ms/thirdpartynotices.\r\n\r\n\r\n    2. ADDITIONAL LICENSING REQUIREMENTS AND/OR USE RIGHTS.\r\n        a. Distributable Code. The software contains code that you are permitted to distribute in programs you develop if you comply with the terms below.\r\n            i. Right to Use and Distribute. The code and test files listed below are “Distributable Code”.\r\n                • REDIST.TXT Files. You may copy and distribute the object code form of code listed in REDIST.TXT files plus the files listed on the REDIST.TXT list located at http://go.microsoft.com/fwlink/?LinkId=524842. Depending on the specific edition of the software, the number of REDIST files you receive with the software may not be equal to the number of REDIST files listed in the REDIST.TXT List. We may add additional files to the list from time to time.\r\n                • Third Party Distribution. You may permit distributors of your programs to copy and distribute the Distributable Code as part of those programs. \r\n            ii. Distribution Requirements. For any Distributable Code you distribute, you must\r\n                • Add significant primary functionality to it in your programs;\r\n                • For any Distributable Code having a filename extension of .lib, distribute only the results of running such Distributable Code through a linker with your program;\r\n                • Distribute Distributable Code included in a setup program only as part of that setup program without modification;\r\n                • Require distributors and external end users to agree to terms that protect it at least as much as this agreement;\r\n                • For Distributable Code from the Windows Performance Toolkit portions of the software, distribute the unmodified software package as a whole with your programs, with the exception of the KernelTraceControl.dll and the WindowsPerformanceRecorderControl.dll which can be distributed with your programs;\r\n                • Display your valid copyright notice on your programs; and\r\n                • Indemnify, defend, and hold harmless Microsoft from any claims, including attorneys’ fees, related to the distribution or use of your programs. \r\n            iii. Distribution Restrictions. You may not\r\n                • Alter any copyright, trademark or patent notice in the Distributable Code;\r\n                • Use Microsoft’s trademarks in your programs’ names or in a way that suggests your programs come from or are endorsed by Microsoft;\r\n                • Distribute partial copies of the Windows Performance Toolkit portion of the software package with the exception of the KernelTraceControl.dll and the WindowsPerformanceRecorderControl.dll which can be distributed with your programs;\r\n                • Distribute Distributable Code to run on a platform other than the Microsoft operating system platform;\r\n                • Include Distributable Code in malicious, deceptive or unlawful programs; or\r\n                • Modified or distribute the source code of any Distributable Code so that any part of it becomes subject to an Excluded License. And Excluded License is on that requir3es, as a condition of use, modification or distribution, that\r\n                        ▪ The code be disclosed or distributed in source code form; or\r\n                        ▪ Others have the right to modify it.\r\n        b. Additional Rights and Restrictions for Features made Available with the Software. \r\n            i. Windows App Requirements. If you intend to make your program available in the Windows Store, the program must comply with the Certification Requirements as defined and described in the App Developer Agreement, currently available at: https://msdn.microsoft.com/en-us/library/windows/apps/hh694058.aspx. \r\n            ii. Bing Maps. The software may include features that retrieve content such as maps, images and other data through the Bing Maps (or successor branded) application programming interface (the “Bing Maps API”) to create reports displaying data on top of maps, aerial and hybrid imagery. If these features are included, you may use these features to create and view dynamic or static documents only in conjunction with and through methods and means of access integrated in the software. You may not otherwise copy, store, archive, or create a database of the entity information including business names, addresses and geocodes available through the Bing Maps API. You may not use the Bing Maps API to provide sensor based guidance/routing, nor use any Road Traffic Data or Bird’s Eye Imager (or associated metadata) even if available through the Bing Maps API for any purpose. Your use of the Bing Maps API and associated content is also subject to the additional terms and conditions at http://go.microsoft.com/fwlink/?LinkId=21969.\r\n            iii. Additional Mapping APIs. The software may include application programming interfaces that provide maps and other related mapping features and services that are not provided by Bing (the “Additional Mapping APIs”). These Additional Mapping APIs are subject to additional terms and conditions and may require payment of fees to Microsoft and/or third party providers based on the use or volume of use of such Additional Mapping APIs. These terms and conditions will be provided when you obtain any necessary license keys to use such Additional Mapping APIs or when you review or receive documentation related to the use of such Additional Mapping APIs.\r\n            iv. Push Notifications. The Microsoft Push Notification Service may not be used to send notifications that are mission critical or otherwise could affect matters of life or death, including without limitation critical notifications related to a medical device or condition. MICROSOFT EXPRESSLY DISCLAIMS ANY WARRANTIES THAT THE USE OF THE MICROSOFT PUSH NOTIFICATION SERVICE OR DELIVERY OF MICROSOFT PUSH NOTIFICATION SERVICE NOTIFICATIONS WILL BE UNINTERRUPTED, ERROR FREE, OR OTHERWISE GUARANTEED TO OCCUR ON A REAL-TIME BASIS.\r\n            v. Speech namespace API. Using speech recognition functionality via the Speech namespace APIs in a program requires the support of a speech recognition service. The service may require network connectivity at the time of recognition (e.g., when using a predefined grammar). In addition, the service may also collect speech-related data in order to provide and improve the service. The speech-related data may include, for example, information related to grammar size and string phrases in a grammar.\r\n\tAlso, in order for a user to use speech recognition on the phone they must first accept certain terms of use. The terms of use notify the user that data related to their use of the speech recognition service will be collected and used to provide and improve the service. If a user does not accept the terms of use and speech recognition is attempted by the application, the operation will not work and an error will be returned to the application. \r\n            vi. PlayReady Support. The software may include the Windows Emulator, which contains Microsoft’s PlayReady content access technology.  Content owners use Microsoft PlayReady content access technology to protect their intellectual property, including copyrighted content.  This software uses PlayReady technology to access PlayReady-protected content and/or WMDRM-protected content.  Microsoft may decide to revoke the software’s ability to consume PlayReady-protected content for reasons including but not limited to (i) if a breach or potential breach of PlayReady technology occurs, (ii) proactive robustness enhancement, and (iii) if Content owners require the revocation because the software fails to properly enforce restrictions on content usage.  Revocation should not affect unprotected content or content protected by other content access technologies.  Content owners may require you to upgrade PlayReady to access their content.  If you decline an upgrade, you will not be able to access content that requires the upgrade and may not be able to install other operating system updates or upgrades.  \r\n            vii. Package Managers. The software may include package managers, like NuGet, that give you the option to download other Microsoft and third party software packages to use with your application. Those packages are under their own licenses, and not this agreement. Microsoft does not distribute, license or provide any warranties for any of the third party packages.\r\n            viii. Font Components. While the software is running, you may use its fonts to display and print content. You may only embed fonts in content as permitted by the embedding restrictions in the fonts; and temporarily download them to a printer or other output device to help print content. \r\n            ix. Notice about the H.264/AVD Visual Standard, and the VC-1 Video Standard. This software may include H.264/MPEG-4 AVC and/or VD-1 decoding technology. MPEG LA, L.L.C. requires this notice: \r\nTHIS PRODUCT IS LICENSED UNDER THE AVC AND THE VC-1 PATENT PORTFOLIO LICENSES FOR THE PERSONAL AND NON-COMMERCIAL USE OF A CONSUMER TO (i) ENCODE VIDEO IN COMPLIANCE WITH THE ABOVE STANDARDS (“VIDEO STANDARDS”) AND/OR (ii) DECODE AVC, AND VC-1 VIDEO THAT WAS ENCODED BY A CONSUMER ENGAGED IN A PERSONAL AND NON-COMMERCIAL ACTIVITY AND/OR WAS OBTAINED FROM A VIDEO PROVIDER LICENSED TO PROVIDE SUCH VIDEO. NONE OF THE LICENSES EXTEND TO ANY OTHER PRODUCT REGARDLESS OF WHETHER SUCH PRODUCT IS INCLUDED WITH THIS SOFTWARE IN A SINGLE ARTICLE. NO LICENSE IS GRANTED OR SHALL BE IMPLIED FOR ANY OTHER USE. ADDITIONAL INFORMATION MAY BE OBTAINED FROM MPEG LA, L.L.C. SEE WWW.MPEGLA.COM.\r\nFor clarification purposes, this notice does not limit or inhibit the use of the software for normal business uses that are personal to that business which do not include (i) redistribution of the software to third parties, or (ii) creation of content with the VIDEO STANDARDS compliant technologies for distribution to third parties.\r\n    3. INTERNET-BASED SERVICES. Microsoft provides Internet-based services with the software. It may change or cancel them at any time. \r\n        a. Consent for Internet-Based Services. The software features described below and in the privacy statement at http://go.microsoft.com/fwlink/?LinkId=521839 connect to Microsoft or service provider computer systems over the Internet. In some cases, you will not receive a separate notice when they connect. In some cases, you may switch off these features or not use them as described in the applicable product documentation. By using these features, you consent to the transmission of this information. Microsoft does not use the information to identify or contact you.\r\n            i. Computer Information. The following features use Internet protocols, which send to the appropriate systems computer information, such as your Internet protocol address, the type of operating system, browser, and name and version of the software you are using, and the language code of the device where you installed the software. Microsoft uses this information to make the Internet-based services available to you.\r\n                • Software Use and Performance.  This software collects info about your hardware and how you use the software and automatically sends error reports to Microsoft.  These reports include information about problems that occur in the software.  Reports might unintentionally contain personal information. For example, a report that contains a snapshot of computer memory might include your name. Part of a document you were working on could be included as well, but this information in reports or any info collected about hardware or your software use will not be used to identify or contact you.\r\n                • Digital Certificates. The software uses digital certificates. These digital certificates confirm the identity of Internet users sending X.509 standard encryption information. They also can be used to digitally sign files and macros to verify the integrity and origin of the file contents. The software retrieves certificates and updates certificate revocation lists using the Internet, when available.\r\n                • Windows Application Certification Kit. To ensure you have the latest certification tests, when launched this software periodically checks a Windows Application Certification Kit file on download.microsft.com to see if an update is available.  If an update is found, you are prompted and provided a link to a web site where you can download the update. You may use the Windows Application Certification Kit solely to test your programs before you submit them for a potential Microsoft Windows Certification and for inclusion on the Microsoft Windows Store. The results you receive are for informational purposes only. Microsoft has no obligation to either (i) provide you with a Windows Certification for your programs and/or ii) include your program in the Microsoft Windows Store.  \r\n                • Microsoft Digital Rights Management for Silverlight. \r\nIf you use Silverlight to access content that has been protected with Microsoft Digital Rights Management (DRM), in order to let you play the content, the software may automatically\r\n                • request media usage rights from a rights server on the Internet and\r\n                • download and install available DRM Updates.\r\nFor more information about this feature, including instructions for turning the Automatic Updates off, go to http://go.microsoft.com/fwlink/?LinkId=147032.\r\n                1. Web Content Features.  Features in the software can retrieve related content from Microsoft and provide it to you. To provide the content, these features send to Microsoft the type of operating system, name and version of the software you are using, type of browser and language code of the device where you installed the software. Examples of these features are clip art, templates, online training, online assistance, help and Appshelp. You may choose not to use these web content features.\r\n            ii. Use of Information. We may use  nformation collected about software use and performance to provide and improve Microsoft software and services as further described in Microsoft’s Privacy Statement available at: https://go.microsoft.com/fwlink/?LinkID=521839. We may also share it with others, such as hardware and software vendors. They may use the information to improve how their products run with Microsoft software.\r\n            iii. Misuse of Internet-based Services. You may not use these services in any way that could harm them or impair anyone else’s use of them. You may not use the services to try to gain unauthorized access to any service, data, account or network by any means. \r\n    4. YOUR COMPLIANCE WITH PRIVACY AND DATA PROTECTION LAWS.\r\n        a. Personal Information Definition. \"Personal Information\" means any information relating to an identified or identifiable natural person; an identifiable natural person is one who can be identified, directly or indirectly, in particular by reference to an identifier such as a name, an identification number, location data, an online identifier or to one or more factors specific to the physical, physiological, genetic, mental, economic, cultural or social identity of that natural person.\r\n        b. Collecting Personal Information using Packaged and Add-on APIs.  If you use any API to collect personal information from the software, you must comply with all laws and regulations applicable to your use of the data accessed through APIs including without limitation laws related to privacy, biometric data, data protection, and confidentiality of communications. Your use of the software is conditioned upon implementing and maintaining appropriate protections and measures for your applications and services, and that includes your responsibility to the data obtained through the use of APIs. For the data you obtained through any APIs, you must: \r\n            i. obtain all necessary consents before collecting and using data and only use the data for the limited purposes to which the user consented, including any consent to changes in use; \r\n            ii. In the event you’re storing data, ensure that data is kept up to date and implement corrections, restrictions to data, or the deletion of data as updated through packaged or add-on APIs or upon user request if required by applicable law;\r\n            iii. implement proper retention and deletion policies, including deleting all data when as directed by your users or as required by applicable law; and\r\n            iv. maintain and comply with a written statement available to your customers that describes your privacy practices regarding data and information you collect, use and that you share with any third parties. \r\n        c. Location Framework. The software may contain a location framework component or APIs that enable support of location services in programs.  Programs that receive device location must comply with the requirements related to the Location Service APIs as described in the Microsoft Store Policies (https://docs.microsoft.com/en-us/legal/windows/agreements/store-policies).   If you choose to collect device location data outside of the control of Windows system settings, you must obtain legally sufficient consent for your data practices, and such practices must comply with all other applicable laws and regulations. \r\n        d. Security.  If your application or service collects, stores or transmits personal information, it must do so securely, by using modern cryptography methods.\r\n    5. BACKUP COPY. You may make one backup copy of the software. You may use it only to reinstall the software.\r\n    6. DOCUMENTATION. Any person that has valid access to your computer or internal network may copy and use the documentation for your internal, reference purposes.\r\n    7. SCOPE OF LICENSE. The software is licensed, not sold. This agreement only gives you some rights to use the software. Microsoft reserves all other rights. Unless applicable law gives you more rights despite this limitation, you may use the software only as expressly permitted in this agreement. In doing so, you must comply with any technical limitations in the software that only allow you to use it in certain ways. You may not\r\n    • Except for the Microsoft .NET Framework, you must obtain Microsoft's prior written approval to disclose to a third party the results of any benchmark test of the software.\r\n    • work around any technical limitations in the software;\r\n    • reverse engineer, decompile or disassemble the software, except and only to the extent that applicable law expressly permits, despite this limitation;\r\n    • make more copies of the software than specified in this agreement or allowed by applicable law, despite this limitation;\r\n    • publish the software for others to copy;\r\n    • rent, lease or lend the software;\r\n    • transfer the software or this agreement to any third party; or\r\n    • use the software for commercial software hosting services.\r\n    8. EXPORT RESTRICTIONS. The software is subject to United States export laws and regulations. You must comply with all domestic and international export laws and regulations that apply to the software. These laws include restrictions on destinations, end users and end use. For additional information, see www.microsoft.com/exporting.\r\n    9. SUPPORT SERVICES. Because this software is “as is,” we may not provide support services for it.\r\n    10. ENTIRE AGREEMENT. This agreement, and the terms for supplements, updates, Internet-based services and support services that you use, are the entire agreement for the software and support services.\r\n    11. INDEPENDENT PARTIES.  Microsoft and you are independent contractors. Nothing in this agreement shall be construed as creating an employer-employee relationship, processor-subprocessor relationship, a partnership, or a joint venture between the parties.\r\n    12. APPLICABLE LAW AND PLACE TO RESOLVE DISPUTES. If you acquired the software in the United States or Canada, the laws of the state or province where you live (or, if a business, where your principal place of business is located) govern the interpretation of this agreement, claims for its breach, and all other claims (including consumer protection, unfair competition, and tort claims), regardless of conflict of laws principles. If you acquired the software in any other country, its laws apply. If U.S. federal jurisdiction exists, you and Microsoft consent to exclusive jurisdiction and venue in the federal court in King County, Washington for all disputes heard in court. If not, you and Microsoft consent to exclusive jurisdiction and venue in the Superior Court of King County, Washington for all disputes heard in court.\r\n    13. LEGAL EFFECT. This agreement describes certain legal rights. You may have other rights under the laws of your country. You may also have rights with respect to the party from whom you acquired the software. This agreement does not change your rights under the laws of your country if the laws of your country do not permit it to do so. \r\n    14. DISCLAIMER OF WARRANTY. The software is licensed “as-is.” You bear the risk of using it. Microsoft gives no express warranties, guarantees or conditions. You may have additional consumer rights or statutory guarantees under your local laws which this agreement cannot change. To the extent permitted under your local laws, Microsoft excludes the implied warranties of merchantability, fitness for a particular purpose and non-infringement.\r\nFOR AUSTRALIA – You have statutory guarantees under the Australian Consumer Law and nothing in these terms is intended to affect those rights.\r\n    15. LIMITATION ON AND EXCLUSION OF REMEDIES AND DAMAGES. You can recover from Microsoft and its suppliers only direct damages up to U.S. $5.00. You cannot recover any other damages, including consequential, lost profits, special, indirect or incidental damages.\r\nThis limitation applies to\r\n    • anything related to the software, services, content (including code) on third party Internet sites, or third party programs; and\r\n    • claims for breach of contract, breach of warranty, guarantee or condition, strict liability, negligence, or other tort to the extent permitted by applicable law.\r\nIt also applies even if Microsoft knew or should have known about the possibility of the damages. The above limitation or exclusion may not apply to you because your country may not allow the exclusion or limitation of incidental, consequential or other damages.\r\n\r\nPlease note: As this software is distributed in Quebec, Canada, some of the clauses in this agreement are provided below in French.\r\nRemarque : Ce logiciel étant distribué au Québec, Canada, certaines des clauses dans ce contrat sont fournies ci-dessous en français.\r\nEXONÉRATION DE GARANTIE. Le logiciel visé par une licence est offert « tel quel ». Toute utilisation de ce logiciel est à votre seule risque et péril. Microsoft n’accorde aucune autre garantie expresse. Vous pouvez bénéficier de droits additionnels en vertu du droit local sur la protection des consommateurs, que ce contrat ne peut modifier. La ou elles sont permises par le droit locale, les garanties implicites de qualité marchande, d’adéquation à un usage particulier et d’absence de contrefaçon sont exclues.\r\nLIMITATION DES DOMMAGES-INTÉRÊTS ET EXCLUSION DE RESPONSABILITÉ POUR LES DOMMAGES. Vous pouvez obtenir de Microsoft et de ses fournisseurs une indemnisation en cas de dommages directs uniquement à hauteur de 5,00 $ US. Vous ne pouvez prétendre à aucune indemnisation pour les autres dommages, y compris les dommages spéciaux, indirects ou accessoires et pertes de bénéfices.\r\nCrete limitation concern:\r\n    • tout ce qui est relié au logiciel, aux services ou au contenu (y compris le code) figurant sur des sites Internet tiers ou dans des programmes tiers ; et\r\n    • les réclamations au titre de violation de contrat ou de garantie, ou au titre de responsabilité stricte, de négligence ou d’une autre faute dans la limite autorisée par la loi en vigueur.\r\nElle s’applique également, même si Microsoft connaissait ou devrait connaître l’éventualité d’un tel dommage. Si votre pays n’autorise pas l’exclusion ou la limitation de responsabilité pour les dommages indirects, accessoires ou de quelque nature que ce soit, il se peut que la limitation ou l’exclusion ci-dessus ne s’appliquera pas à votre égard.\r\nEFFET JURIDIQUE. Le présent contrat décrit certains droits juridiques. Vous pourriez avoir d’autres droits prévus par les lois de votre pays. Le présent contrat ne modifie pas les droits que vous confèrent les lois de votre pays si celles-ci ne le permettent pas.\r\n***************\r\nEULAID:WIN10SDK.RTM.AUG_2018_en-US\r\n\r\n\r\n*************************************************************************"},
                            new TextBlock { FontSize=16,Margin = new Microsoft.UI.Xaml.Thickness(0, 0, 0, 10),Text = "Microsoft.WindowsAppSDK 1.7.250606001" },
                            new TextBlock { FontSize=9,Margin = new Microsoft.UI.Xaml.Thickness(0, 0, 0, 20),TextWrapping=TextWrapping.Wrap ,Text = "MICROSOFT SOFTWARE LICENSE TERMS\r\nMICROSOFT WINDOWS APP SDK\r\n________________________________________\r\nIF YOU LIVE IN (OR ARE A BUSINESS WITH A PRINCIPAL PLACE OF BUSINESS IN) THE UNITED STATES, PLEASE READ THE “BINDING ARBITRATION AND CLASS ACTION WAIVER” SECTION BELOW. IT AFFECTS HOW DISPUTES ARE RESOLVED.\r\n________________________________________\r\n\r\nThese license terms are an agreement between you and Microsoft Corporation (or one of its affiliates). They apply to the software named above and any Microsoft services or software updates (except to the extent such services or updates are accompanied by new or additional terms, in which case those different terms apply prospectively and do not alter your or Microsoft’s rights relating to pre-updated software or services). IF YOU COMPLY WITH THESE LICENSE TERMS, YOU HAVE THE RIGHTS BELOW.  BY USING THE SOFTWARE, YOU ACCEPT THESE TERMS.\r\n\r\n1. INSTALLATION AND USE RIGHTS.\r\n\r\n    a) General. Subject to the terms of this agreement, you may install and use any number of copies of the software to develop and test your applications, solely for use on Windows.\r\n\r\n    b) Included Microsoft Applications. The software may include other Microsoft applications. These license terms apply to those included applications, if any, unless other license terms are provided with the other Microsoft applications.\r\n\r\n    c) Microsoft Platforms. The software may include components from Microsoft Windows. These components are governed by separate agreements and their own product support policies, as described in the license terms found in the installation directory for that component or in the “Licenses” folder accompanying the software.\r\n\r\n2. DATA.\r\n\r\n    a) Data Collection. The software may collect information about you and your use of the software, and send that to Microsoft. Microsoft may use this information to provide services and improve our products and services. You may opt-out of many of these scenarios, but not all, as described in the product documentation.  There are also some features in the software that may enable you to collect data from users of your applications. If you use these features to enable data collection in your applications, you must comply with applicable law, including providing appropriate notices to users of your applications. You can learn more about data collection and use in the help documentation and the privacy statement at https://aka.ms/privacy. Your use of the software operates as your consent to these practices.\r\n\r\n    b) Processing of Personal Data. To the extent Microsoft is a processor or subprocessor of personal data in connection with the software, Microsoft makes the commitments in the European Union General Data Protection Regulation Terms of the Online Services Terms to all customers effective May 25, 2018, at https://docs.microsoft.com/en-us/legal/gdpr.\r\n\r\n3. DISTRIBUTABLE CODE. The software may contain code you are permitted to distribute (i.e. make available for third parties) in applications you develop, as described in this Section.\r\n\r\n    a) Distribution Rights. The code and test files described below are distributable if included with the software.\r\n\r\n        i. Image Library. You may copy and distribute images, graphics, and animations in the Image Library as described in the software documentation; and\r\n\r\n        ii. Third Party Distribution. You may permit distributors of your applications to copy and distribute any of this distributable code you elect to distribute with your applications.\r\n\r\n    b) Distribution Requirements. For any code you distribute, you must:\r\n\r\n        i. add significant primary functionality to it in your applications;\r\n\r\n        ii. require distributors and external end users to agree to terms that protect it and Microsoft at least as much as this agreement; and\r\n\r\n        iii. indemnify, defend, and hold harmless Microsoft from any claims, including attorneys’ fees, related to the distribution or use of your applications, except to the extent that any claim is based solely on the unmodified distributable code.\r\n\r\n    c) Distribution Restrictions. You may not:\r\n\r\n        i. use Microsoft’s trademarks or trade dress in your application in any way that suggests your application comes from or is endorsed by Microsoft; or\r\n\r\n        ii. modify or distribute the source code of any distributable code so that any part of it becomes subject to any license that requires that the distributable code, any other part of the software, or any of Microsoft’s other intellectual property be disclosed or distributed in source code form, or that others have the right to modify it.\r\n\r\n4. SCOPE OF LICENSE. The software is licensed, not sold. Microsoft reserves all other rights. Unless applicable law gives you more rights despite this limitation, you will not (and have no right to):\r\n\r\n    a) work around any technical limitations in the software that only allow you to use it in certain ways;\r\n\r\n    b) reverse engineer, decompile or disassemble the software, or otherwise attempt to derive the source code for the software, except and to the extent required by third party licensing terms governing use of certain open source components that may be included in the software;\r\n\r\n    c) remove, minimize, block, or modify any notices of Microsoft or its suppliers in the software;\r\n\r\n    d) use the software in any way that is against the law or to create or propagate malware; or\r\n\r\n    e) share, publish, distribute, or lease the software (except for any distributable code, subject to the terms above), provide the software as a stand-alone offering for others to use, or transfer the software or this agreement to any third party.\r\n\r\n5. EXPORT RESTRICTIONS. You must comply with all domestic and international export laws and regulations that apply to the software, which include restrictions on destinations, end users, and end use. For further information on export restrictions, visit https://aka.ms/exporting.\r\n\r\n6. SUPPORT SERVICES. Microsoft is not obligated under this agreement to provide any support services for the software. Any support provided is “as is”, “with all faults”, and without warranty of any kind.\r\n\r\n7. UPDATES. The software may periodically check for updates, and download and install them for you. You may obtain updates only from Microsoft or authorized sources. Microsoft may need to update your system to provide you with updates. You agree to receive these automatic updates without any additional notice. Updates may not include or support all existing software features, services, or peripheral devices.\r\n\r\n8. BINDING ARBITRATION AND CLASS ACTION WAIVER. This Section applies if you live in (or, if a business, your principal place of business is in) the United States.  If you and Microsoft have a dispute, you and Microsoft agree to try for 60 days to resolve it informally. If you and Microsoft can’t, you and Microsoft agree to binding individual arbitration before the American Arbitration Association under the Federal Arbitration Act (“FAA”), and not to sue in court in front of a judge or jury. Instead, a neutral arbitrator will decide. Class action lawsuits, class-wide arbitrations, private attorney-general actions, and any other proceeding where someone acts in a representative capacity are not allowed; nor is combining individual proceedings without the consent of all parties. The complete Arbitration Agreement contains more terms and is at https://aka.ms/arb-agreement-4. You and Microsoft agree to these terms.\r\n\r\n9. ENTIRE AGREEMENT. This agreement, and any other terms Microsoft may provide for supplements, updates, or third-party applications, is the entire agreement for the software.\r\n\r\n10. APPLICABLE LAW AND PLACE TO RESOLVE DISPUTES. If you acquired the software in the United States or Canada, the laws of the state or province where you live (or, if a business, where your principal place of business is located) govern the interpretation of this agreement, claims for its breach, and all other claims (including consumer protection, unfair competition, and tort claims), regardless of conflict of laws principles, except that the FAA governs everything related to arbitration. If you acquired the software in any other country, its laws apply, except that the FAA governs everything related to arbitration. If U.S. federal jurisdiction exists, you and Microsoft consent to exclusive jurisdiction and venue in the federal court in King County, Washington for all disputes heard in court (excluding arbitration). If not, you and Microsoft consent to exclusive jurisdiction and venue in the Superior Court of King County, Washington for all disputes heard in court (excluding arbitration).\r\n\r\n11. CONSUMER RIGHTS; REGIONAL VARIATIONS. This agreement describes certain legal rights. You may have other rights, including consumer rights, under the laws of your state or country. Separate and apart from your relationship with Microsoft, you may also have rights with respect to the party from which you acquired the software. This agreement does not change those other rights if the laws of your state or country do not permit it to do so. For example, if you acquired the software in one of the below regions, or mandatory country law applies, then the following provisions apply to you:\r\n\r\n    a) Australia. You have statutory guarantees under the Australian Consumer Law and nothing in this agreement is intended to affect those rights.\r\n\r\n    b) Canada. If you acquired this software in Canada, you may stop receiving updates by turning off the automatic update feature, disconnecting your device from the Internet (if and when you re-connect to the Internet, however, the software will resume checking for and installing updates), or uninstalling the software. The product documentation, if any, may also specify how to turn off updates for your specific device or software.\r\n\r\n    c) Germany and Austria.\r\n\r\n        i. Warranty. The properly licensed software will perform substantially as described in any Microsoft materials that accompany the software. However, Microsoft gives no contractual guarantee in relation to the licensed software.\r\n\r\n        ii. Limitation of Liability. In case of intentional conduct, gross negligence, claims based on the Product Liability Act, as well as, in case of death or personal or physical injury, Microsoft is liable according to the statutory law.\r\n\r\n        Subject to the foregoing clause ii., Microsoft will only be liable for slight negligence if Microsoft is in breach of such material contractual obligations, the fulfillment of which facilitate the due performance of this agreement, the breach of which would endanger the purpose of this agreement and the compliance with which a party may constantly trust in (so-called \"cardinal obligations\"). In other cases of slight negligence, Microsoft will not be liable for slight negligence.\r\n\r\n12. DISCLAIMER OF WARRANTY. THE SOFTWARE IS LICENSED “AS IS.” YOU BEAR THE RISK OF USING IT. MICROSOFT GIVES NO EXPRESS WARRANTIES, GUARANTEES, OR CONDITIONS. TO THE EXTENT PERMITTED UNDER APPLICABLE LAWS, MICROSOFT EXCLUDES ALL IMPLIED WARRANTIES, INCLUDING MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE, AND NON-INFRINGEMENT.\r\n\r\n13. LIMITATION ON AND EXCLUSION OF DAMAGES. IF YOU HAVE ANY BASIS FOR RECOVERING DAMAGES DESPITE THE PRECEDING DISCLAIMER OF WARRANTY, YOU CAN RECOVER FROM MICROSOFT AND ITS SUPPLIERS ONLY DIRECT DAMAGES UP TO U.S. $5.00. YOU CANNOT RECOVER ANY OTHER DAMAGES, INCLUDING CONSEQUENTIAL, LOST PROFITS, SPECIAL, INDIRECT, OR INCIDENTAL DAMAGES.\r\n\r\nThis limitation applies to (a) anything related to the software, services, content (including code) on third party Internet sites, or third party applications; and (b) claims for breach of contract, warranty, guarantee, or condition; strict liability, negligence, or other tort; or any other claim; in each case to the extent permitted by applicable law.\r\n\r\nIt also applies even if Microsoft knew or should have known about the possibility of the damages. The above limitation or exclusion may not apply to you because your state, province, or country may not allow the exclusion or limitation of incidental, consequential, or other damages.\r\n"},
                            new TextBlock { FontSize=16,Margin = new Microsoft.UI.Xaml.Thickness(0, 0, 0, 10),Text = "Microsoft.Web.WebView2 1.0.3351.48" },
                            new TextBlock { FontSize=9,Margin = new Microsoft.UI.Xaml.Thickness(0, 0, 0, 20),TextWrapping=TextWrapping.Wrap ,Text = "Copyright (C) Microsoft Corporation. All rights reserved.\r\n\r\nRedistribution and use in source and binary forms, with or without\r\nmodification, are permitted provided that the following conditions are\r\nmet:\r\n\r\n   * Redistributions of source code must retain the above copyright\r\nnotice, this list of conditions and the following disclaimer.\r\n   * Redistributions in binary form must reproduce the above\r\ncopyright notice, this list of conditions and the following disclaimer\r\nin the documentation and/or other materials provided with the\r\ndistribution.\r\n   * The name of Microsoft Corporation, or the names of its contributors \r\nmay not be used to endorse or promote products derived from this\r\nsoftware without specific prior written permission.\r\n\r\nTHIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS\r\n\"AS IS\" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT\r\nLIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR\r\nA PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT\r\nOWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,\r\nSPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT\r\nLIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,\r\nDATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY\r\nTHEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT\r\n(INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE\r\nOF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE."},
                            new TextBlock { FontSize=16,Margin = new Microsoft.UI.Xaml.Thickness(0, 0, 0, 10),Text = "NVIDIA CUDA Toolkit 12.9" },
                            new TextBlock { FontSize=9,Margin = new Microsoft.UI.Xaml.Thickness(0, 0, 0, 20),TextWrapping=TextWrapping.Wrap ,Text = "End User License Agreement\r\n--------------------------\r\n\r\nNVIDIA Software License Agreement and CUDA Supplement to\r\nSoftware License Agreement.\r\n\r\nThe CUDA Toolkit End User License Agreement applies to the\r\nNVIDIA CUDA Toolkit, the NVIDIA CUDA Samples, the NVIDIA\r\nDisplay Driver, NVIDIA Nsight tools (Visual Studio Edition),\r\nand the associated documentation on CUDA APIs, programming\r\nmodel and development tools. If you do not agree with the\r\nterms and conditions of the license agreement, then do not\r\ndownload or use the software.\r\n\r\nLast updated: January 12, 2024.\r\n\r\n\r\nPreface\r\n-------\r\n\r\nThe Software License Agreement in Chapter 1 and the Supplement\r\nin Chapter 2 contain license terms and conditions that govern\r\nthe use of NVIDIA toolkit. By accepting this agreement, you\r\nagree to comply with all the terms and conditions applicable\r\nto the product(s) included herein.\r\n\r\n\r\nNVIDIA Driver\r\n\r\n\r\nDescription\r\n\r\nThis package contains the operating system driver and\r\nfundamental system software components for NVIDIA GPUs.\r\n\r\n\r\nNVIDIA CUDA Toolkit\r\n\r\n\r\nDescription\r\n\r\nThe NVIDIA CUDA Toolkit provides command-line and graphical\r\ntools for building, debugging and optimizing the performance\r\nof applications accelerated by NVIDIA GPUs, runtime and math\r\nlibraries, and documentation including programming guides,\r\nuser manuals, and API references.\r\n\r\n\r\nDefault Install Location of CUDA Toolkit\r\n\r\nWindows platform:\r\n\r\n%ProgramFiles%\\NVIDIA GPU Computing Toolkit\\CUDA\\v#.#\r\n\r\nLinux platform:\r\n\r\n/usr/local/cuda-#.#\r\n\r\nMac platform:\r\n\r\n/Developer/NVIDIA/CUDA-#.#\r\n\r\n\r\nNVIDIA CUDA Samples\r\n\r\n\r\nDescription\r\n\r\nCUDA Samples are now located in\r\nhttps://github.com/nvidia/cuda-samples, which includes\r\ninstructions for obtaining, building, and running the samples.\r\nThey are no longer included in the CUDA toolkit.\r\n\r\n\r\nNVIDIA Nsight Visual Studio Edition (Windows only)\r\n\r\n\r\nDescription\r\n\r\nNVIDIA Nsight Development Platform, Visual Studio Edition is a\r\ndevelopment environment integrated into Microsoft Visual\r\nStudio that provides tools for debugging, profiling, analyzing\r\nand optimizing your GPU computing and graphics applications.\r\n\r\n\r\nDefault Install Location of Nsight Visual Studio Edition\r\n\r\nWindows platform:\r\n\r\n%ProgramFiles(x86)%\\NVIDIA Corporation\\Nsight Visual Studio Edition #.#\r\n\r\n\r\n1. License Agreement for NVIDIA Software Development Kits\r\n---------------------------------------------------------\r\n\r\n\r\nImportant Notice—Read before downloading, installing,\r\ncopying or using the licensed software:\r\n-------------------------------------------------------\r\n\r\nThis license agreement, including exhibits attached\r\n(\"Agreement”) is a legal agreement between you and NVIDIA\r\nCorporation (\"NVIDIA\") and governs your use of a NVIDIA\r\nsoftware development kit (“SDK”).\r\n\r\nEach SDK has its own set of software and materials, but here\r\nis a description of the types of items that may be included in\r\na SDK: source code, header files, APIs, data sets and assets\r\n(examples include images, textures, models, scenes, videos,\r\nnative API input/output files), binary software, sample code,\r\nlibraries, utility programs, programming code and\r\ndocumentation.\r\n\r\nThis Agreement can be accepted only by an adult of legal age\r\nof majority in the country in which the SDK is used.\r\n\r\nIf you are entering into this Agreement on behalf of a company\r\nor other legal entity, you represent that you have the legal\r\nauthority to bind the entity to this Agreement, in which case\r\n“you” will mean the entity you represent.\r\n\r\nIf you don’t have the required age or authority to accept\r\nthis Agreement, or if you don’t accept all the terms and\r\nconditions of this Agreement, do not download, install or use\r\nthe SDK.\r\n\r\nYou agree to use the SDK only for purposes that are permitted\r\nby (a) this Agreement, and (b) any applicable law, regulation\r\nor generally accepted practices or guidelines in the relevant\r\njurisdictions.\r\n\r\n\r\n1.1. License\r\n\r\n\r\n1.1.1. License Grant\r\n\r\nSubject to the terms of this Agreement, NVIDIA hereby grants\r\nyou a non-exclusive, non-transferable license, without the\r\nright to sublicense (except as expressly provided in this\r\nAgreement) to:\r\n\r\n  1. Install and use the SDK,\r\n\r\n  2. Modify and create derivative works of sample source code\r\n    delivered in the SDK, and\r\n\r\n  3. Distribute those portions of the SDK that are identified\r\n    in this Agreement as distributable, as incorporated in\r\n    object code format into a software application that meets\r\n    the distribution requirements indicated in this Agreement.\r\n\r\n\r\n1.1.2. Distribution Requirements\r\n\r\nThese are the distribution requirements for you to exercise\r\nthe distribution grant:\r\n\r\n  1. Your application must have material additional\r\n    functionality, beyond the included portions of the SDK.\r\n\r\n  2. The distributable portions of the SDK shall only be\r\n    accessed by your application.\r\n\r\n  3. The following notice shall be included in modifications\r\n    and derivative works of sample source code distributed:\r\n    “This software contains source code provided by NVIDIA\r\n    Corporation.”\r\n\r\n  4. Unless a developer tool is identified in this Agreement\r\n    as distributable, it is delivered for your internal use\r\n    only.\r\n\r\n  5. The terms under which you distribute your application\r\n    must be consistent with the terms of this Agreement,\r\n    including (without limitation) terms relating to the\r\n    license grant and license restrictions and protection of\r\n    NVIDIA’s intellectual property rights. Additionally, you\r\n    agree that you will protect the privacy, security and\r\n    legal rights of your application users.\r\n\r\n  6. You agree to notify NVIDIA in writing of any known or\r\n    suspected distribution or use of the SDK not in compliance\r\n    with the requirements of this Agreement, and to enforce\r\n    the terms of your agreements with respect to distributed\r\n    SDK.\r\n\r\n\r\n1.1.3. Authorized Users\r\n\r\nYou may allow employees and contractors of your entity or of\r\nyour subsidiary(ies) to access and use the SDK from your\r\nsecure network to perform work on your behalf.\r\n\r\nIf you are an academic institution you may allow users\r\nenrolled or employed by the academic institution to access and\r\nuse the SDK from your secure network.\r\n\r\nYou are responsible for the compliance with the terms of this\r\nAgreement by your authorized users. If you become aware that\r\nyour authorized users didn’t follow the terms of this\r\nAgreement, you agree to take reasonable steps to resolve the\r\nnon-compliance and prevent new occurrences.\r\n\r\n\r\n1.1.4. Pre-Release SDK\r\n\r\nThe SDK versions identified as alpha, beta, preview or\r\notherwise as pre-release, may not be fully functional, may\r\ncontain errors or design flaws, and may have reduced or\r\ndifferent security, privacy, accessibility, availability, and\r\nreliability standards relative to commercial versions of\r\nNVIDIA software and materials. Use of a pre-release SDK may\r\nresult in unexpected results, loss of data, project delays or\r\nother unpredictable damage or loss.\r\n\r\nYou may use a pre-release SDK at your own risk, understanding\r\nthat pre-release SDKs are not intended for use in production\r\nor business-critical systems.\r\n\r\nNVIDIA may choose not to make available a commercial version\r\nof any pre-release SDK. NVIDIA may also choose to abandon\r\ndevelopment and terminate the availability of a pre-release\r\nSDK at any time without liability.\r\n\r\n\r\n1.1.5. Updates\r\n\r\nNVIDIA may, at its option, make available patches, workarounds\r\nor other updates to this SDK. Unless the updates are provided\r\nwith their separate governing terms, they are deemed part of\r\nthe SDK licensed to you as provided in this Agreement. You\r\nagree that the form and content of the SDK that NVIDIA\r\nprovides may change without prior notice to you. While NVIDIA\r\ngenerally maintains compatibility between versions, NVIDIA may\r\nin some cases make changes that introduce incompatibilities in\r\nfuture versions of the SDK.\r\n\r\n\r\n1.1.6. Components Under Other Licenses\r\n\r\nThe SDK may come bundled with, or otherwise include or be\r\ndistributed with, NVIDIA or third-party components with\r\nseparate legal notices or terms as may be described in\r\nproprietary notices accompanying the SDK. If and to the extent\r\nthere is a conflict between the terms in this Agreement and\r\nthe license terms associated with the component, the license\r\nterms associated with the components control only to the\r\nextent necessary to resolve the conflict.\r\n\r\nSubject to the other terms of this Agreement, you may use the\r\nSDK to develop and test applications released under Open\r\nSource Initiative (OSI) approved open source software\r\nlicenses.\r\n\r\n\r\n1.1.7. Reservation of Rights\r\n\r\nNVIDIA reserves all rights, title, and interest in and to the\r\nSDK, not expressly granted to you under this Agreement.\r\n\r\n\r\n1.2. Limitations\r\n\r\nThe following license limitations apply to your use of the\r\nSDK:\r\n\r\n  1. You may not reverse engineer, decompile or disassemble,\r\n    or remove copyright or other proprietary notices from any\r\n    portion of the SDK or copies of the SDK.\r\n\r\n  2. Except as expressly provided in this Agreement, you may\r\n    not copy, sell, rent, sublicense, transfer, distribute,\r\n    modify, or create derivative works of any portion of the\r\n    SDK. For clarity, you may not distribute or sublicense the\r\n    SDK as a stand-alone product.\r\n\r\n  3. Unless you have an agreement with NVIDIA for this\r\n    purpose, you may not indicate that an application created\r\n    with the SDK is sponsored or endorsed by NVIDIA.\r\n\r\n  4. You may not bypass, disable, or circumvent any\r\n    encryption, security, digital rights management or\r\n    authentication mechanism in the SDK.\r\n\r\n  5. You may not use the SDK in any manner that would cause it\r\n    to become subject to an open source software license. As\r\n    examples, licenses that require as a condition of use,\r\n    modification, and/or distribution that the SDK be:\r\n\r\n      a. Disclosed or distributed in source code form;\r\n\r\n      b. Licensed for the purpose of making derivative works;\r\n        or\r\n\r\n      c. Redistributable at no charge.\r\n\r\n  6.  You acknowledge that the SDK as delivered is not tested\r\n    or certified by NVIDIA for use in connection with the\r\n    design, construction, maintenance, and/or operation of any\r\n    system where the use or failure of such system could\r\n    result in a situation that threatens the safety of human\r\n    life or results in catastrophic damages (each, a \"Critical\r\n    Application\"). Examples of Critical Applications include\r\n    use in avionics, navigation, autonomous vehicle\r\n    applications, ai solutions for automotive products,\r\n    military, medical, life support or other life critical\r\n    applications. NVIDIA shall not be liable to you or any\r\n    third party, in whole or in part, for any claims or\r\n    damages arising from such uses. You are solely responsible\r\n    for ensuring that any product or service developed with\r\n    the SDK as a whole includes sufficient features to comply\r\n    with all applicable legal and regulatory standards and\r\n    requirements.\r\n\r\n  7.  You agree to defend, indemnify and hold harmless NVIDIA\r\n    and its affiliates, and their respective employees,\r\n    contractors, agents, officers and directors, from and\r\n    against any and all claims, damages, obligations, losses,\r\n    liabilities, costs or debt, fines, restitutions and\r\n    expenses (including but not limited to attorney’s fees\r\n    and costs incident to establishing the right of\r\n    indemnification) arising out of or related to products or\r\n    services that use the SDK in or for Critical Applications,\r\n    and for use of the SDK outside of the scope of this\r\n    Agreement or not in compliance with its terms.\r\n\r\n  8. You may not reverse engineer, decompile or disassemble\r\n    any portion of the output generated using SDK elements for\r\n    the purpose of translating such output artifacts to target\r\n    a non-NVIDIA platform.\r\n\r\n\r\n1.3. Ownership\r\n\r\n  1.  NVIDIA or its licensors hold all rights, title and\r\n    interest in and to the SDK and its modifications and\r\n    derivative works, including their respective intellectual\r\n    property rights, subject to your rights under Section\r\n    1.3.2. This SDK may include software and materials from\r\n    NVIDIA’s licensors, and these licensors are intended\r\n    third party beneficiaries that may enforce this Agreement\r\n    with respect to their intellectual property rights.\r\n\r\n  2.  You hold all rights, title and interest in and to your\r\n    applications and your derivative works of the sample\r\n    source code delivered in the SDK, including their\r\n    respective intellectual property rights, subject to\r\n    NVIDIA’s rights under Section 1.3.1.\r\n\r\n  3. You may, but don’t have to, provide to NVIDIA\r\n    suggestions, feature requests or other feedback regarding\r\n    the SDK, including possible enhancements or modifications\r\n    to the SDK. For any feedback that you voluntarily provide,\r\n    you hereby grant NVIDIA and its affiliates a perpetual,\r\n    non-exclusive, worldwide, irrevocable license to use,\r\n    reproduce, modify, license, sublicense (through multiple\r\n    tiers of sublicensees), and distribute (through multiple\r\n    tiers of distributors) it without the payment of any\r\n    royalties or fees to you. NVIDIA will use feedback at its\r\n    choice. NVIDIA is constantly looking for ways to improve\r\n    its products, so you may send feedback to NVIDIA through\r\n    the developer portal at https://developer.nvidia.com.\r\n\r\n\r\n1.4. No Warranties\r\n\r\nTHE SDK IS PROVIDED BY NVIDIA “AS IS” AND “WITH ALL\r\nFAULTS.” TO THE MAXIMUM EXTENT PERMITTED BY LAW, NVIDIA AND\r\nITS AFFILIATES EXPRESSLY DISCLAIM ALL WARRANTIES OF ANY KIND\r\nOR NATURE, WHETHER EXPRESS, IMPLIED OR STATUTORY, INCLUDING,\r\nBUT NOT LIMITED TO, ANY WARRANTIES OF MERCHANTABILITY, FITNESS\r\nFOR A PARTICULAR PURPOSE, TITLE, NON-INFRINGEMENT, OR THE\r\nABSENCE OF ANY DEFECTS THEREIN, WHETHER LATENT OR PATENT. NO\r\nWARRANTY IS MADE ON THE BASIS OF TRADE USAGE, COURSE OF\r\nDEALING OR COURSE OF TRADE.\r\n\r\n\r\n1.5. Limitation of Liability\r\n\r\nTO THE MAXIMUM EXTENT PERMITTED BY LAW, NVIDIA AND ITS\r\nAFFILIATES SHALL NOT BE LIABLE FOR ANY (I) SPECIAL, INCIDENTAL,\r\nPUNITIVE OR CONSEQUENTIAL DAMAGES, OR (II) DAMAGES FOR (A) ANY \r\nLOST PROFITS, LOSS OF USE, LOSS OF DATA OR LOSS OF GOODWILL, \r\nOR THE COSTS OF PROCURING SUBSTITUTE PRODUCTS, ARISING OUT OF \r\nOR IN CONNECTION WITH THIS AGREEMENT OR THE USE OR PERFORMANCE \r\nOF THE SDK, WHETHER SUCH LIABILITY ARISES FROM ANY CLAIM BASED \r\nUPON BREACH OF CONTRACT, BREACH OF WARRANTY, TORT (INCLUDING \r\nNEGLIGENCE), PRODUCT LIABILITY OR ANY OTHER CAUSE OF ACTION OR \r\nTHEORY OF LIABILITY. IN NO EVENT WILL NVIDIA’S AND ITS AFFILIATES\r\nTOTAL CUMULATIVE LIABILITY UNDER OR ARISING OUT OF THIS\r\nAGREEMENT EXCEED US$10.00. THE NATURE OF THE LIABILITY OR THE\r\nNUMBER OF CLAIMS OR SUITS SHALL NOT ENLARGE OR EXTEND THIS\r\nLIMIT.\r\n\r\nThese exclusions and limitations of liability shall apply\r\nregardless if NVIDIA or its affiliates have been advised of\r\nthe possibility of such damages, and regardless of whether a\r\nremedy fails its essential purpose. These exclusions and\r\nlimitations of liability form an essential basis of the\r\nbargain between the parties, and, absent any of these\r\nexclusions or limitations of liability, the provisions of this\r\nAgreement, including, without limitation, the economic terms,\r\nwould be substantially different.\r\n\r\n\r\n1.6. Termination\r\n\r\n  1. This Agreement will continue to apply until terminated by\r\n    either you or NVIDIA as described below.\r\n\r\n  2. If you want to terminate this Agreement, you may do so by\r\n    stopping to use the SDK.\r\n\r\n  3. NVIDIA may, at any time, terminate this Agreement if:\r\n\r\n      a. (i) you fail to comply with any term of this\r\n        Agreement and the non-compliance is not fixed within\r\n        thirty (30) days following notice from NVIDIA (or\r\n        immediately if you violate NVIDIA’s intellectual\r\n        property rights);\r\n\r\n      b. (ii) you commence or participate in any legal\r\n        proceeding against NVIDIA with respect to the SDK; or\r\n\r\n      c. (iii) NVIDIA decides to no longer provide the SDK in\r\n        a country or, in NVIDIA’s sole discretion, the\r\n        continued use of it is no longer commercially viable.\r\n\r\n  4. Upon any termination of this Agreement, you agree to\r\n    promptly discontinue use of the SDK and destroy all copies\r\n    in your possession or control. Your prior distributions in\r\n    accordance with this Agreement are not affected by the\r\n    termination of this Agreement. Upon written request, you\r\n    will certify in writing that you have complied with your\r\n    commitments under this section. Upon any termination of\r\n    this Agreement all provisions survive except for the\r\n    license grant provisions.\r\n\r\n\r\n1.7. General\r\n\r\nIf you wish to assign this Agreement or your rights and\r\nobligations, including by merger, consolidation, dissolution\r\nor operation of law, contact NVIDIA to ask for permission. Any\r\nattempted assignment not approved by NVIDIA in writing shall\r\nbe void and of no effect. NVIDIA may assign, delegate or\r\ntransfer this Agreement and its rights and obligations, and if\r\nto a non-affiliate you will be notified.\r\n\r\nYou agree to cooperate with NVIDIA and provide reasonably\r\nrequested information to verify your compliance with this\r\nAgreement.\r\n\r\nThis Agreement will be governed in all respects by the laws of\r\nthe United States and of the State of Delaware, without regard to the\r\nconflicts of laws principles. The United Nations Convention on\r\nContracts for the International Sale of Goods is specifically\r\ndisclaimed. You agree to all terms of this Agreement in the\r\nEnglish language.\r\n\r\nThe state or federal courts residing in Santa Clara County,\r\nCalifornia shall have exclusive jurisdiction over any dispute\r\nor claim arising out of this Agreement. Notwithstanding this,\r\nyou agree that NVIDIA shall still be allowed to apply for\r\ninjunctive remedies or an equivalent type of urgent legal\r\nrelief in any jurisdiction.\r\n\r\nIf any court of competent jurisdiction determines that any\r\nprovision of this Agreement is illegal, invalid or\r\nunenforceable, such provision will be construed as limited to\r\nthe extent necessary to be consistent with and fully\r\nenforceable under the law and the remaining provisions will\r\nremain in full force and effect. Unless otherwise specified,\r\nremedies are cumulative.\r\n\r\nEach party acknowledges and agrees that the other is an\r\nindependent contractor in the performance of this Agreement.\r\n\r\nThe SDK has been developed entirely at private expense and is\r\n“commercial items” consisting of “commercial computer\r\nsoftware” and “commercial computer software\r\ndocumentation” provided with RESTRICTED RIGHTS. Use,\r\nduplication or disclosure by the U.S. Government or a U.S.\r\nGovernment subcontractor is subject to the restrictions in\r\nthis Agreement pursuant to DFARS 227.7202-3(a) or as set forth\r\nin subparagraphs (c)(1) and (2) of the Commercial Computer\r\nSoftware - Restricted Rights clause at FAR 52.227-19, as\r\napplicable. Contractor/manufacturer is NVIDIA, 2788 San Tomas\r\nExpressway, Santa Clara, CA 95051.\r\n\r\nThe SDK is subject to United States export laws and\r\nregulations. You agree that you will not ship, transfer or\r\nexport the SDK into any country, or use the SDK in any manner,\r\nprohibited by the United States Bureau of Industry and\r\nSecurity or economic sanctions regulations administered by the\r\nU.S. Department of Treasury’s Office of Foreign Assets\r\nControl (OFAC), or any applicable export laws, restrictions or\r\nregulations. These laws include restrictions on destinations,\r\nend users and end use. By accepting this Agreement, you\r\nconfirm that you are not located in a country currently \r\nembargoed by the U.S. or otherwise prohibited from receiving \r\nthe SDK under U.S. law.\r\n\r\nAny notice delivered by NVIDIA to you under this Agreement\r\nwill be delivered via mail, email or fax. You agree that any\r\nnotices that NVIDIA sends you electronically will satisfy any\r\nlegal communication requirements. Please direct your legal\r\nnotices or other correspondence to NVIDIA Corporation, 2788\r\nSan Tomas Expressway, Santa Clara, California 95051, United\r\nStates of America, Attention: Legal Department.\r\n\r\nThis Agreement and any exhibits incorporated into this\r\nAgreement constitute the entire agreement of the parties with\r\nrespect to the subject matter of this Agreement and supersede\r\nall prior negotiations or documentation exchanged between the\r\nparties relating to this SDK license. Any additional and/or\r\nconflicting terms on documents issued by you are null, void,\r\nand invalid. Any amendment or waiver under this Agreement\r\nshall be in writing and signed by representatives of both\r\nparties.\r\n\r\n\r\n2. CUDA Toolkit Supplement to Software License Agreement for\r\nNVIDIA Software Development Kits\r\n------------------------------------------------------------\r\n\r\nThe terms in this supplement govern your use of the NVIDIA\r\nCUDA Toolkit SDK under the terms of your license agreement\r\n(“Agreement”) as modified by this supplement. Capitalized\r\nterms used but not defined below have the meaning assigned to\r\nthem in the Agreement.\r\n\r\nThis supplement is an exhibit to the Agreement and is\r\nincorporated as an integral part of the Agreement. In the\r\nevent of conflict between the terms in this supplement and the\r\nterms in the Agreement, the terms in this supplement govern.\r\n\r\n\r\n2.1. License Scope\r\n\r\nThe SDK is licensed for you to develop applications only for\r\nuse in systems with NVIDIA GPUs.\r\n\r\n\r\n2.2. Distribution\r\n\r\nThe portions of the SDK that are distributable under the\r\nAgreement are listed in Attachment A.\r\n\r\n\r\n2.3. Operating Systems\r\n\r\nThose portions of the SDK designed exclusively for use on the\r\nLinux or FreeBSD operating systems, or other operating systems\r\nderived from the source code to these operating systems, may\r\nbe copied and redistributed for use in accordance with this\r\nAgreement, provided that the object code files are not\r\nmodified in any way (except for unzipping of compressed\r\nfiles).\r\n\r\n\r\n2.4. Audio and Video Encoders and Decoders\r\n\r\nYou acknowledge and agree that it is your sole responsibility\r\nto obtain any additional third-party licenses required to\r\nmake, have made, use, have used, sell, import, and offer for\r\nsale your products or services that include or incorporate any\r\nthird-party software and content relating to audio and/or\r\nvideo encoders and decoders from, including but not limited\r\nto, Microsoft, Thomson, Fraunhofer IIS, Sisvel S.p.A.,\r\nMPEG-LA, and Coding Technologies. NVIDIA does not grant to you\r\nunder this Agreement any necessary patent or other rights with\r\nrespect to any audio and/or video encoders and decoders.\r\n\r\n\r\n2.5. Licensing\r\n\r\nIf the distribution terms in this Agreement are not suitable\r\nfor your organization, or for any questions regarding this\r\nAgreement, please contact NVIDIA at\r\nnvidia-compute-license-questions@nvidia.com.\r\n\r\n\r\n2.6. Attachment A\r\n\r\nThe following CUDA Toolkit files may be distributed with\r\napplications developed by you, including certain\r\nvariations of these files that have version number or\r\narchitecture specific information embedded in the file name -\r\nas an example only, for release version 9.0 of the 64-bit\r\nWindows software, the file cudart64_90.dll is redistributable.\r\n\r\nComponent\r\n\r\nCUDA Runtime\r\n\r\nWindows\r\n\r\ncudart.dll, cudart_static.lib, cudadevrt.lib\r\n\r\nMac OSX\r\n\r\nlibcudart.dylib, libcudart_static.a, libcudadevrt.a\r\n\r\nLinux\r\n\r\nlibcudart.so, libcudart_static.a, libcudadevrt.a\r\n\r\nAndroid\r\n\r\nlibcudart.so, libcudart_static.a, libcudadevrt.a\r\n\r\nComponent\r\n\r\nCUDA FFT Library\r\n\r\nWindows\r\n\r\ncufft.dll, cufftw.dll, cufft.lib, cufftw.lib\r\n\r\nMac OSX\r\n\r\nlibcufft.dylib, libcufft_static.a, libcufftw.dylib,\r\nlibcufftw_static.a\r\n\r\nLinux\r\n\r\nlibcufft.so, libcufft_static.a, libcufftw.so,\r\nlibcufftw_static.a\r\n\r\nAndroid\r\n\r\nlibcufft.so, libcufft_static.a, libcufftw.so,\r\nlibcufftw_static.a\r\n\r\nComponent\r\n\r\nCUDA BLAS Library\r\n\r\nWindows\r\n\r\ncublas.dll, cublasLt.dll\r\n\r\nMac OSX\r\n\r\nlibcublas.dylib, libcublasLt.dylib, libcublas_static.a,\r\nlibcublasLt_static.a\r\n\r\nLinux\r\n\r\nlibcublas.so, libcublasLt.so, libcublas_static.a,\r\nlibcublasLt_static.a\r\n\r\nAndroid\r\n\r\nlibcublas.so, libcublasLt.so, libcublas_static.a,\r\nlibcublasLt_static.a\r\n\r\nComponent\r\n\r\nNVIDIA \"Drop-in\" BLAS Library\r\n\r\nWindows\r\n\r\nnvblas.dll\r\n\r\nMac OSX\r\n\r\nlibnvblas.dylib\r\n\r\nLinux\r\n\r\nlibnvblas.so\r\n\r\nComponent\r\n\r\nCUDA Sparse Matrix Library\r\n\r\nWindows\r\n\r\ncusparse.dll, cusparse.lib\r\n\r\nMac OSX\r\n\r\nlibcusparse.dylib, libcusparse_static.a\r\n\r\nLinux\r\n\r\nlibcusparse.so, libcusparse_static.a\r\n\r\nAndroid\r\n\r\nlibcusparse.so, libcusparse_static.a\r\n\r\nComponent\r\n\r\nCUDA Linear Solver Library\r\n\r\nWindows\r\n\r\ncusolver.dll, cusolver.lib\r\n\r\nMac OSX\r\n\r\nlibcusolver.dylib, libcusolver_static.a\r\n\r\nLinux\r\n\r\nlibcusolver.so, libcusolver_static.a\r\n\r\nAndroid\r\n\r\nlibcusolver.so, libcusolver_static.a\r\n\r\nComponent\r\n\r\nCUDA Random Number Generation Library\r\n\r\nWindows\r\n\r\ncurand.dll, curand.lib\r\n\r\nMac OSX\r\n\r\nlibcurand.dylib, libcurand_static.a\r\n\r\nLinux\r\n\r\nlibcurand.so, libcurand_static.a\r\n\r\nAndroid\r\n\r\nlibcurand.so, libcurand_static.a\r\n\r\nComponent\r\n\r\nNVIDIA Performance Primitives Library\r\n\r\nWindows\r\n\r\nnppc.dll, nppc.lib, nppial.dll, nppial.lib, nppicc.dll,\r\nnppicc.lib, nppicom.dll, nppicom.lib, nppidei.dll,\r\nnppidei.lib, nppif.dll, nppif.lib, nppig.dll, nppig.lib,\r\nnppim.dll, nppim.lib, nppist.dll, nppist.lib, nppisu.dll,\r\nnppisu.lib, nppitc.dll, nppitc.lib, npps.dll, npps.lib\r\n\r\nMac OSX\r\n\r\nlibnppc.dylib, libnppc_static.a, libnppial.dylib,\r\nlibnppial_static.a, libnppicc.dylib, libnppicc_static.a,\r\nlibnppicom.dylib, libnppicom_static.a, libnppidei.dylib,\r\nlibnppidei_static.a, libnppif.dylib, libnppif_static.a,\r\nlibnppig.dylib, libnppig_static.a, libnppim.dylib,\r\nlibnppisu_static.a, libnppitc.dylib, libnppitc_static.a,\r\nlibnpps.dylib, libnpps_static.a\r\n\r\nLinux\r\n\r\nlibnppc.so, libnppc_static.a, libnppial.so,\r\nlibnppial_static.a, libnppicc.so, libnppicc_static.a,\r\nlibnppicom.so, libnppicom_static.a, libnppidei.so,\r\nlibnppidei_static.a, libnppif.so, libnppif_static.a\r\nlibnppig.so, libnppig_static.a, libnppim.so,\r\nlibnppim_static.a, libnppist.so, libnppist_static.a,\r\nlibnppisu.so, libnppisu_static.a, libnppitc.so\r\nlibnppitc_static.a, libnpps.so, libnpps_static.a\r\n\r\nAndroid\r\n\r\nlibnppc.so, libnppc_static.a, libnppial.so,\r\nlibnppial_static.a, libnppicc.so, libnppicc_static.a,\r\nlibnppicom.so, libnppicom_static.a, libnppidei.so,\r\nlibnppidei_static.a, libnppif.so, libnppif_static.a\r\nlibnppig.so, libnppig_static.a, libnppim.so,\r\nlibnppim_static.a, libnppist.so, libnppist_static.a,\r\nlibnppisu.so, libnppisu_static.a, libnppitc.so\r\nlibnppitc_static.a, libnpps.so, libnpps_static.a\r\n\r\nComponent\r\n\r\nNVIDIA JPEG Library\r\n\r\nWindows\r\n\r\nnvjpeg.lib, nvjpeg.dll\r\n\r\nLinux\r\n\r\nlibnvjpeg.so, libnvjpeg_static.a\r\n\r\nComponent\r\n\r\nInternal common library required for statically linking to\r\ncuBLAS, cuSPARSE, cuFFT, cuRAND, nvJPEG and NPP\r\n\r\nMac OSX\r\n\r\nlibculibos.a\r\n\r\nLinux\r\n\r\nlibculibos.a\r\n\r\nComponent\r\n\r\nNVIDIA Runtime Compilation Library and Header\r\n\r\nAll\r\n\r\nnvrtc.h\r\n\r\nWindows\r\n\r\nnvrtc.dll, nvrtc-builtins.dll\r\n\r\nMac OSX\r\n\r\nlibnvrtc.dylib, libnvrtc-builtins.dylib\r\n\r\nLinux\r\n\r\nlibnvrtc.so, libnvrtc-builtins.so, libnvrtc_static.a, libnvrtx-builtins_static.a\r\n\r\nComponent\r\n\r\nNVIDIA Optimizing Compiler Library\r\n\r\nWindows\r\n\r\nnvvm.dll\r\n\r\nMac OSX\r\n\r\nlibnvvm.dylib\r\n\r\nLinux\r\n\r\nlibnvvm.so\r\n\r\nComponent\r\n\r\nNVIDIA JIT Linking Library\r\n\r\nWindows\r\n\r\nlibnvJitLink.dll, libnvJitLink.lib\r\n\r\nLinux\r\n\r\nlibnvJitLink.so, libnvJitLink_static.a\r\n\r\nComponent\r\n\r\nNVIDIA Common Device Math Functions Library\r\n\r\nWindows\r\n\r\nlibdevice.10.bc\r\n\r\nMac OSX\r\n\r\nlibdevice.10.bc\r\n\r\nLinux\r\n\r\nlibdevice.10.bc\r\n\r\nComponent\r\n\r\nCUDA Occupancy Calculation Header Library\r\n\r\nAll\r\n\r\ncuda_occupancy.h\r\n\r\nComponent\r\n\r\nCUDA Half Precision Headers\r\n\r\nAll\r\n\r\ncuda_fp16.h, cuda_fp16.hpp\r\n\r\nComponent\r\n\r\nCUDA Profiling Tools Interface (CUPTI) Library\r\n\r\nWindows\r\n\r\ncupti.dll\r\n\r\nMac OSX\r\n\r\nlibcupti.dylib\r\n\r\nLinux\r\n\r\nlibcupti.so\r\n\r\nComponent\r\n\r\nNVIDIA Tools Extension Library\r\n\r\nWindows\r\n\r\nnvToolsExt.dll, nvToolsExt.lib\r\n\r\nMac OSX\r\n\r\nlibnvToolsExt.dylib\r\n\r\nLinux\r\n\r\nlibnvToolsExt.so\r\n\r\nComponent\r\n\r\nNVIDIA CUDA Driver Libraries\r\n\r\nLinux\r\n\r\nlibcuda.so, libnvidia-ptxjitcompiler.so, libnvptxcompiler_static.a\r\n\r\nComponent\r\n\r\nNVIDIA CUDA File IO Libraries and Header\r\n\r\nAll\r\n\r\ncufile.h\r\n\r\nLinux\r\n\r\nlibcufile.so, libcufile_rdma.so, libcufile_static.a,\r\nlibcufile_rdma_static.a\r\n\r\nIn addition to the rights above, for parties that are\r\ndeveloping software intended solely for use on Jetson\r\ndevelopment kits or Jetson modules, and running Linux for\r\nTegra software, the following shall apply:\r\n\r\n  * The SDK may be distributed in its entirety, as provided by\r\n    NVIDIA, and without separation of its components, for you\r\n    and/or your licensees to create software development kits\r\n    for use only on the Jetson platform and running Linux for\r\n    Tegra software.\r\n\r\n\r\n2.7. Attachment B\r\n\r\n\r\nAdditional Licensing Obligations\r\n\r\nThe following third party components included in the SOFTWARE\r\nare licensed to Licensee pursuant to the following terms and\r\nconditions:\r\n\r\n  1. Licensee's use of the GDB third party component is\r\n    subject to the terms and conditions of GNU GPL v3:\r\n\r\n    This product includes copyrighted third-party software licensed\r\n    under the terms of the GNU General Public License v3 (\"GPL v3\").\r\n    All third-party software packages are copyright by their respective\r\n    authors. GPL v3 terms and conditions are hereby incorporated into\r\n    the Agreement by this reference:     http://www.gnu.org/licenses/gpl.txt\r\n\r\n    Consistent with these licensing requirements, the software\r\n    listed below is provided under the terms of the specified\r\n    open source software licenses. To obtain source code for\r\n    software provided under licenses that require\r\n    redistribution of source code, including the GNU General\r\n    Public License (GPL) and GNU Lesser General Public License\r\n    (LGPL), contact oss-requests@nvidia.com. This offer is\r\n    valid for a period of three (3) years from the date of the\r\n    distribution of this product by NVIDIA CORPORATION.\r\n\r\n    Component          License\r\n    CUDA-GDB           GPL v3  \r\n\r\n  2. Licensee represents and warrants that any and all third\r\n    party licensing and/or royalty payment obligations in\r\n    connection with Licensee's use of the H.264 video codecs\r\n    are solely the responsibility of Licensee.\r\n\r\n  3. Licensee's use of the Thrust library is subject to the\r\n    terms and conditions of the Apache License Version 2.0.\r\n    All third-party software packages are copyright by their\r\n    respective authors. Apache License Version 2.0 terms and\r\n    conditions are hereby incorporated into the Agreement by\r\n    this reference.\r\n    http://www.apache.org/licenses/LICENSE-2.0.html\r\n\r\n    In addition, Licensee acknowledges the following notice:\r\n    Thrust includes source code from the Boost Iterator,\r\n    Tuple, System, and Random Number libraries.\r\n\r\n    Boost Software License - Version 1.0 - August 17th, 2003\r\n    . . . .\r\n    \r\n    Permission is hereby granted, free of charge, to any person or \r\n    organization obtaining a copy of the software and accompanying \r\n    documentation covered by this license (the \"Software\") to use, \r\n    reproduce, display, distribute, execute, and transmit the Software, \r\n    and to prepare derivative works of the Software, and to permit \r\n    third-parties to whom the Software is furnished to do so, all \r\n    subject to the following:\r\n    \r\n    The copyright notices in the Software and this entire statement, \r\n    including the above license grant, this restriction and the following \r\n    disclaimer, must be included in all copies of the Software, in whole \r\n    or in part, and all derivative works of the Software, unless such \r\n    copies or derivative works are solely in the form of machine-executable \r\n    object code generated by a source language processor.\r\n    \r\n    THE SOFTWARE IS PROVIDED \"AS IS\", WITHOUT WARRANTY OF ANY KIND, \r\n    EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF \r\n    MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE, TITLE AND \r\n    NON-INFRINGEMENT. IN NO EVENT SHALL THE COPYRIGHT HOLDERS OR \r\n    ANYONE DISTRIBUTING THE SOFTWARE BE LIABLE FOR ANY DAMAGES OR \r\n    OTHER LIABILITY, WHETHER IN CONTRACT, TORT OR OTHERWISE, ARISING \r\n    FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR \r\n    OTHER DEALINGS IN THE SOFTWARE.  \r\n\r\n  4. Licensee's use of the LLVM third party component is\r\n    subject to the following terms and conditions:\r\n\r\n    ======================================================\r\n    LLVM Release License\r\n    ======================================================\r\n    University of Illinois/NCSA\r\n    Open Source License\r\n    \r\n    Copyright (c) 2003-2010 University of Illinois at Urbana-Champaign.\r\n    All rights reserved.\r\n    \r\n    Developed by:\r\n    \r\n        LLVM Team\r\n    \r\n        University of Illinois at Urbana-Champaign\r\n    \r\n        http://llvm.org\r\n    \r\n    Permission is hereby granted, free of charge, to any person obtaining a copy\r\n    of this software and associated documentation files (the \"Software\"), to \r\n    deal with the Software without restriction, including without limitation the\r\n    rights to use, copy, modify, merge, publish, distribute, sublicense, and/or \r\n    sell copies of the Software, and to permit persons to whom the Software is \r\n    furnished to do so, subject to the following conditions:\r\n    \r\n    *  Redistributions of source code must retain the above copyright notice, \r\n       this list of conditions and the following disclaimers.\r\n    \r\n    *  Redistributions in binary form must reproduce the above copyright \r\n       notice, this list of conditions and the following disclaimers in the \r\n       documentation and/or other materials provided with the distribution.\r\n    \r\n    *  Neither the names of the LLVM Team, University of Illinois at Urbana-\r\n       Champaign, nor the names of its contributors may be used to endorse or\r\n       promote products derived from this Software without specific prior \r\n       written permission.\r\n    \r\n    THE SOFTWARE IS PROVIDED \"AS IS\", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR\r\n    IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY, \r\n    FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL \r\n    THE CONTRIBUTORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR \r\n    OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE,\r\n    ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER\r\n    DEALINGS WITH THE SOFTWARE.  \r\n\r\n  5. Licensee's use of the PCRE third party component is\r\n    subject to the following terms and conditions:\r\n\r\n    ------------\r\n    PCRE LICENCE\r\n    ------------\r\n    PCRE is a library of functions to support regular expressions whose syntax\r\n    and semantics are as close as possible to those of the Perl 5 language.\r\n    Release 8 of PCRE is distributed under the terms of the \"BSD\" licence, as\r\n    specified below. The documentation for PCRE, supplied in the \"doc\" \r\n    directory, is distributed under the same terms as the software itself. The\r\n    basic library functions are written in C and are freestanding. Also \r\n    included in the distribution is a set of C++ wrapper functions, and a just-\r\n    in-time compiler that can be used to optimize pattern matching. These are \r\n    both optional features that can be omitted when the library is built.\r\n    \r\n    THE BASIC LIBRARY FUNCTIONS\r\n    ---------------------------\r\n    Written by:       Philip Hazel\r\n    Email local part: ph10\r\n    Email domain:     cam.ac.uk\r\n    University of Cambridge Computing Service,\r\n    Cambridge, England.\r\n    Copyright (c) 1997-2012 University of Cambridge\r\n    All rights reserved.\r\n    \r\n    PCRE JUST-IN-TIME COMPILATION SUPPORT\r\n    -------------------------------------\r\n    Written by:       Zoltan Herczeg\r\n    Email local part: hzmester\r\n    Emain domain:     freemail.hu\r\n    Copyright(c) 2010-2012 Zoltan Herczeg\r\n    All rights reserved.\r\n    \r\n    STACK-LESS JUST-IN-TIME COMPILER\r\n    --------------------------------\r\n    Written by:       Zoltan Herczeg\r\n    Email local part: hzmester\r\n    Emain domain:     freemail.hu\r\n    Copyright(c) 2009-2012 Zoltan Herczeg\r\n    All rights reserved.\r\n    \r\n    THE C++ WRAPPER FUNCTIONS\r\n    -------------------------\r\n    Contributed by:   Google Inc.\r\n    Copyright (c) 2007-2012, Google Inc.\r\n    All rights reserved.\r\n\r\n    THE \"BSD\" LICENCE\r\n    -----------------\r\n    Redistribution and use in source and binary forms, with or without\r\n    modification, are permitted provided that the following conditions are met:\r\n    \r\n      * Redistributions of source code must retain the above copyright notice, \r\n        this list of conditions and the following disclaimer.\r\n    \r\n      * Redistributions in binary form must reproduce the above copyright \r\n        notice, this list of conditions and the following disclaimer in the \r\n        documentation and/or other materials provided with the distribution.\r\n    \r\n      * Neither the name of the University of Cambridge nor the name of Google \r\n        Inc. nor the names of their contributors may be used to endorse or \r\n        promote products derived from this software without specific prior \r\n        written permission.\r\n    \r\n    THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS \"AS IS\"\r\n    AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE \r\n    IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE \r\n    ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE \r\n    LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR \r\n    CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF \r\n    SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS \r\n    INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN \r\n    CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) \r\n    ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE \r\n    POSSIBILITY OF SUCH DAMAGE.  \r\n\r\n  6. Some of the cuBLAS library routines were written by or\r\n    derived from code written by Vasily Volkov and are subject\r\n    to the Modified Berkeley Software Distribution License as\r\n    follows:\r\n\r\n    Copyright (c) 2007-2009, Regents of the University of California\r\n    \r\n    All rights reserved.\r\n    \r\n    Redistribution and use in source and binary forms, with or without\r\n    modification, are permitted provided that the following conditions are\r\n    met:\r\n        * Redistributions of source code must retain the above copyright\r\n          notice, this list of conditions and the following disclaimer.\r\n        * Redistributions in binary form must reproduce the above\r\n          copyright notice, this list of conditions and the following\r\n          disclaimer in the documentation and/or other materials provided\r\n          with the distribution.\r\n        * Neither the name of the University of California, Berkeley nor\r\n          the names of its contributors may be used to endorse or promote\r\n          products derived from this software without specific prior\r\n          written permission.\r\n    \r\n    THIS SOFTWARE IS PROVIDED BY THE AUTHOR \"AS IS\" AND ANY EXPRESS OR\r\n    IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED\r\n    WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE\r\n    DISCLAIMED. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR ANY DIRECT,\r\n    INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES\r\n    (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR\r\n    SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)\r\n    HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT,\r\n    STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING\r\n    IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE\r\n    POSSIBILITY OF SUCH DAMAGE.  \r\n\r\n  7. Some of the cuBLAS library routines were written by or\r\n    derived from code written by Davide Barbieri and are\r\n    subject to the Modified Berkeley Software Distribution\r\n    License as follows:\r\n\r\n    Copyright (c) 2008-2009 Davide Barbieri @ University of Rome Tor Vergata.\r\n    \r\n    All rights reserved.\r\n    \r\n    Redistribution and use in source and binary forms, with or without\r\n    modification, are permitted provided that the following conditions are\r\n    met:\r\n        * Redistributions of source code must retain the above copyright\r\n          notice, this list of conditions and the following disclaimer.\r\n        * Redistributions in binary form must reproduce the above\r\n          copyright notice, this list of conditions and the following\r\n          disclaimer in the documentation and/or other materials provided\r\n          with the distribution.\r\n        * The name of the author may not be used to endorse or promote\r\n          products derived from this software without specific prior\r\n          written permission.\r\n    \r\n    THIS SOFTWARE IS PROVIDED BY THE AUTHOR \"AS IS\" AND ANY EXPRESS OR\r\n    IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED\r\n    WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE\r\n    DISCLAIMED. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR ANY DIRECT,\r\n    INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES\r\n    (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR\r\n    SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)\r\n    HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT,\r\n    STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING\r\n    IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE\r\n    POSSIBILITY OF SUCH DAMAGE.  \r\n\r\n  8. Some of the cuBLAS library routines were derived from\r\n    code developed by the University of Tennessee and are\r\n    subject to the Modified Berkeley Software Distribution\r\n    License as follows:\r\n\r\n    Copyright (c) 2010 The University of Tennessee.\r\n    \r\n    All rights reserved.\r\n    \r\n    Redistribution and use in source and binary forms, with or without\r\n    modification, are permitted provided that the following conditions are\r\n    met:\r\n        * Redistributions of source code must retain the above copyright\r\n          notice, this list of conditions and the following disclaimer.\r\n        * Redistributions in binary form must reproduce the above\r\n          copyright notice, this list of conditions and the following\r\n          disclaimer listed in this license in the documentation and/or\r\n          other materials provided with the distribution.\r\n        * Neither the name of the copyright holders nor the names of its\r\n          contributors may be used to endorse or promote products derived\r\n          from this software without specific prior written permission.\r\n    \r\n    THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS\r\n    \"AS IS\" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT\r\n    LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR\r\n    A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT\r\n    OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,\r\n    SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT\r\n    LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,\r\n    DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY\r\n    THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT\r\n    (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE\r\n    OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.  \r\n\r\n  9. Some of the cuBLAS library routines were written by or\r\n    derived from code written by Jonathan Hogg and are subject\r\n    to the Modified Berkeley Software Distribution License as\r\n    follows:\r\n\r\n    Copyright (c) 2012, The Science and Technology Facilities Council (STFC).\r\n    \r\n    All rights reserved.\r\n    \r\n    Redistribution and use in source and binary forms, with or without\r\n    modification, are permitted provided that the following conditions are\r\n    met:\r\n        * Redistributions of source code must retain the above copyright\r\n          notice, this list of conditions and the following disclaimer.\r\n        * Redistributions in binary form must reproduce the above\r\n          copyright notice, this list of conditions and the following\r\n          disclaimer in the documentation and/or other materials provided\r\n          with the distribution.\r\n        * Neither the name of the STFC nor the names of its contributors\r\n          may be used to endorse or promote products derived from this\r\n          software without specific prior written permission.\r\n    \r\n    THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS\r\n    \"AS IS\" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT\r\n    LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR\r\n    A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE STFC BE\r\n    LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR\r\n    CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF\r\n    SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR\r\n    BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY,\r\n    WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE\r\n    OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN\r\n    IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.  \r\n\r\n  10. Some of the cuBLAS library routines were written by or\r\n    derived from code written by Ahmad M. Abdelfattah, David\r\n    Keyes, and Hatem Ltaief, and are subject to the Apache\r\n    License, Version 2.0, as follows:\r\n\r\n     -- (C) Copyright 2013 King Abdullah University of Science and Technology\r\n      Authors:\r\n      Ahmad Abdelfattah (ahmad.ahmad@kaust.edu.sa)\r\n      David Keyes (david.keyes@kaust.edu.sa)\r\n      Hatem Ltaief (hatem.ltaief@kaust.edu.sa)\r\n    \r\n      Redistribution  and  use  in  source and binary forms, with or without\r\n      modification,  are  permitted  provided  that the following conditions\r\n      are met:\r\n    \r\n      * Redistributions  of  source  code  must  retain  the above copyright\r\n        notice,  this  list  of  conditions  and  the  following  disclaimer.\r\n      * Redistributions  in  binary  form must reproduce the above copyright\r\n        notice,  this list of conditions and the following disclaimer in the\r\n        documentation  and/or other materials provided with the distribution.\r\n      * Neither  the  name of the King Abdullah University of Science and\r\n        Technology nor the names of its contributors may be used to endorse \r\n        or promote products derived from this software without specific prior \r\n        written permission.\r\n    \r\n      THIS  SOFTWARE  IS  PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS\r\n      ``AS IS''  AND  ANY  EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT\r\n      LIMITED  TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR\r\n      A  PARTICULAR  PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT\r\n      HOLDERS OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,\r\n      SPECIAL,  EXEMPLARY,  OR  CONSEQUENTIAL  DAMAGES  (INCLUDING,  BUT NOT\r\n      LIMITED  TO,  PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,\r\n      DATA,  OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY\r\n      THEORY  OF  LIABILITY,  WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT\r\n      (INCLUDING  NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE\r\n      OF  THIS  SOFTWARE,  EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE  \r\n\r\n  11. Some of the cuSPARSE library routines were written by or\r\n    derived from code written by Li-Wen Chang and are subject\r\n    to the NCSA Open Source License as follows:\r\n\r\n    Copyright (c) 2012, University of Illinois.\r\n    \r\n    All rights reserved.\r\n    \r\n    Developed by: IMPACT Group, University of Illinois, http://impact.crhc.illinois.edu\r\n    \r\n    Permission is hereby granted, free of charge, to any person obtaining\r\n    a copy of this software and associated documentation files (the\r\n    \"Software\"), to deal with the Software without restriction, including\r\n    without limitation the rights to use, copy, modify, merge, publish,\r\n    distribute, sublicense, and/or sell copies of the Software, and to\r\n    permit persons to whom the Software is furnished to do so, subject to\r\n    the following conditions:\r\n        * Redistributions of source code must retain the above copyright\r\n          notice, this list of conditions and the following disclaimer.\r\n        * Redistributions in binary form must reproduce the above\r\n          copyright notice, this list of conditions and the following\r\n          disclaimers in the documentation and/or other materials provided\r\n          with the distribution.\r\n        * Neither the names of IMPACT Group, University of Illinois, nor\r\n          the names of its contributors may be used to endorse or promote\r\n          products derived from this Software without specific prior\r\n          written permission.\r\n    \r\n    THE SOFTWARE IS PROVIDED \"AS IS\", WITHOUT WARRANTY OF ANY KIND,\r\n    EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF\r\n    MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND\r\n    NONINFRINGEMENT. IN NO EVENT SHALL THE CONTRIBUTORS OR COPYRIGHT\r\n    HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER\r\n    IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR\r\n    IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS WITH THE\r\n    SOFTWARE.  \r\n\r\n  12. Some of the cuRAND library routines were written by or\r\n    derived from code written by Mutsuo Saito and Makoto\r\n    Matsumoto and are subject to the following license:\r\n\r\n    Copyright (c) 2009, 2010 Mutsuo Saito, Makoto Matsumoto and Hiroshima\r\n    University. All rights reserved.\r\n    \r\n    Copyright (c) 2011 Mutsuo Saito, Makoto Matsumoto, Hiroshima\r\n    University and University of Tokyo.  All rights reserved.\r\n    \r\n    Redistribution and use in source and binary forms, with or without\r\n    modification, are permitted provided that the following conditions are\r\n    met:\r\n        * Redistributions of source code must retain the above copyright\r\n          notice, this list of conditions and the following disclaimer.\r\n        * Redistributions in binary form must reproduce the above\r\n          copyright notice, this list of conditions and the following\r\n          disclaimer in the documentation and/or other materials provided\r\n          with the distribution.\r\n        * Neither the name of the Hiroshima University nor the names of\r\n          its contributors may be used to endorse or promote products\r\n          derived from this software without specific prior written\r\n          permission.\r\n    \r\n    THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS\r\n    \"AS IS\" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT\r\n    LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR\r\n    A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT\r\n    OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,\r\n    SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT\r\n    LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,\r\n    DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY\r\n    THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT\r\n    (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE\r\n    OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.  \r\n\r\n  13. Some of the cuRAND library routines were derived from\r\n    code developed by D. E. Shaw Research and are subject to\r\n    the following license:\r\n\r\n    Copyright 2010-2011, D. E. Shaw Research.\r\n    \r\n    All rights reserved.\r\n    \r\n    Redistribution and use in source and binary forms, with or without\r\n    modification, are permitted provided that the following conditions are\r\n    met:\r\n        * Redistributions of source code must retain the above copyright\r\n          notice, this list of conditions, and the following disclaimer.\r\n        * Redistributions in binary form must reproduce the above\r\n          copyright notice, this list of conditions, and the following\r\n          disclaimer in the documentation and/or other materials provided\r\n          with the distribution.\r\n        * Neither the name of D. E. Shaw Research nor the names of its\r\n          contributors may be used to endorse or promote products derived\r\n          from this software without specific prior written permission.\r\n    \r\n    THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS\r\n    \"AS IS\" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT\r\n    LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR\r\n    A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT\r\n    OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,\r\n    SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT\r\n    LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,\r\n    DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY\r\n    THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT\r\n    (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE\r\n    OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.  \r\n\r\n  14. Some of the Math library routines were written by or\r\n    derived from code developed by Norbert Juffa and are\r\n    subject to the following license:\r\n\r\n    Copyright (c) 2015-2017, Norbert Juffa\r\n    All rights reserved.\r\n    \r\n    Redistribution and use in source and binary forms, with or without \r\n    modification, are permitted provided that the following conditions\r\n    are met:\r\n    \r\n    1. Redistributions of source code must retain the above copyright \r\n       notice, this list of conditions and the following disclaimer.\r\n    \r\n    2. Redistributions in binary form must reproduce the above copyright\r\n       notice, this list of conditions and the following disclaimer in the\r\n       documentation and/or other materials provided with the distribution.\r\n    \r\n    THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS \r\n    \"AS IS\" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT \r\n    LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR\r\n    A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT\r\n    HOLDER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,\r\n    SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT \r\n    LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,\r\n    DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY\r\n    THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT \r\n    (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE\r\n    OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.  \r\n\r\n  15. Licensee's use of the lz4 third party component is\r\n    subject to the following terms and conditions:\r\n\r\n    Copyright (C) 2011-2013, Yann Collet.\r\n    BSD 2-Clause License (http://www.opensource.org/licenses/bsd-license.php)\r\n    \r\n    Redistribution and use in source and binary forms, with or without\r\n    modification, are permitted provided that the following conditions are\r\n    met:\r\n    \r\n        * Redistributions of source code must retain the above copyright\r\n    notice, this list of conditions and the following disclaimer.\r\n        * Redistributions in binary form must reproduce the above\r\n    copyright notice, this list of conditions and the following disclaimer\r\n    in the documentation and/or other materials provided with the\r\n    distribution.\r\n    \r\n    THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS\r\n    \"AS IS\" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT\r\n    LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR\r\n    A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT\r\n    OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,\r\n    SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT\r\n    LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,\r\n    DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY\r\n    THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT\r\n    (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE\r\n    OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.  \r\n\r\n  16. The NPP library uses code from the Boost Math Toolkit,\r\n    and is subject to the following license:\r\n\r\n    Boost Software License - Version 1.0 - August 17th, 2003\r\n    . . . .\r\n    \r\n    Permission is hereby granted, free of charge, to any person or \r\n    organization obtaining a copy of the software and accompanying \r\n    documentation covered by this license (the \"Software\") to use, \r\n    reproduce, display, distribute, execute, and transmit the Software, \r\n    and to prepare derivative works of the Software, and to permit \r\n    third-parties to whom the Software is furnished to do so, all \r\n    subject to the following:\r\n    \r\n    The copyright notices in the Software and this entire statement, \r\n    including the above license grant, this restriction and the following \r\n    disclaimer, must be included in all copies of the Software, in whole \r\n    or in part, and all derivative works of the Software, unless such \r\n    copies or derivative works are solely in the form of machine-executable \r\n    object code generated by a source language processor.\r\n    \r\n    THE SOFTWARE IS PROVIDED \"AS IS\", WITHOUT WARRANTY OF ANY KIND, \r\n    EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF \r\n    MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE, TITLE AND \r\n    NON-INFRINGEMENT. IN NO EVENT SHALL THE COPYRIGHT HOLDERS OR \r\n    ANYONE DISTRIBUTING THE SOFTWARE BE LIABLE FOR ANY DAMAGES OR \r\n    OTHER LIABILITY, WHETHER IN CONTRACT, TORT OR OTHERWISE, ARISING \r\n    FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR \r\n    OTHER DEALINGS IN THE SOFTWARE.  \r\n\r\n  17. Portions of the Nsight Eclipse Edition is subject to the\r\n    following license:\r\n\r\n    The Eclipse Foundation makes available all content in this plug-in\r\n    (\"Content\"). Unless otherwise indicated below, the Content is provided\r\n    to you under the terms and conditions of the Eclipse Public License\r\n    Version 1.0 (\"EPL\"). A copy of the EPL is available at http://\r\n    www.eclipse.org/legal/epl-v10.html. For purposes of the EPL, \"Program\"\r\n    will mean the Content.\r\n    \r\n    If you did not receive this Content directly from the Eclipse\r\n    Foundation, the Content is being redistributed by another party\r\n    (\"Redistributor\") and different terms and conditions may apply to your\r\n    use of any object code in the Content. Check the Redistributor's\r\n    license that was provided with the Content. If no such license exists,\r\n    contact the Redistributor. Unless otherwise indicated below, the terms\r\n    and conditions of the EPL still apply to any source code in the\r\n    Content and such source code may be obtained at http://www.eclipse.org.  \r\n\r\n  18. Some of the cuBLAS library routines uses code from\r\n    OpenAI, which is subject to the following license:\r\n\r\n    License URL \r\n    https://github.com/openai/openai-gemm/blob/master/LICENSE\r\n    \r\n    License Text \r\n    The MIT License\r\n    \r\n    Copyright (c) 2016 OpenAI (http://openai.com), 2016 Google Inc.\r\n    \r\n    Permission is hereby granted, free of charge, to any person obtaining a copy\r\n    of this software and associated documentation files (the \"Software\"), to deal\r\n    in the Software without restriction, including without limitation the rights\r\n    to use, copy, modify, merge, publish, distribute, sublicense, and/or sell\r\n    copies of the Software, and to permit persons to whom the Software is\r\n    furnished to do so, subject to the following conditions:\r\n    \r\n    The above copyright notice and this permission notice shall be included in\r\n    all copies or substantial portions of the Software.\r\n    \r\n    THE SOFTWARE IS PROVIDED \"AS IS\", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR\r\n    IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,\r\n    FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE\r\n    AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER\r\n    LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,\r\n    OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN\r\n    THE SOFTWARE.   \r\n\r\n  19. Licensee's use of the Visual Studio Setup Configuration\r\n    Samples is subject to the following license:\r\n\r\n    The MIT License (MIT) \r\n    Copyright (C) Microsoft Corporation. All rights reserved.\r\n    \r\n    Permission is hereby granted, free of charge, to any person \r\n    obtaining a copy of this software and associated documentation \r\n    files (the \"Software\"), to deal in the Software without restriction, \r\n    including without limitation the rights to use, copy, modify, merge, \r\n    publish, distribute, sublicense, and/or sell copies of the Software, \r\n    and to permit persons to whom the Software is furnished to do so, \r\n    subject to the following conditions:\r\n    \r\n    The above copyright notice and this permission notice shall be included \r\n    in all copies or substantial portions of the Software.\r\n    \r\n    THE SOFTWARE IS PROVIDED \"AS IS\", WITHOUT WARRANTY OF ANY KIND, EXPRESS \r\n    OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY, \r\n    FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE \r\n    AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER \r\n    LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, \r\n    OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.  \r\n\r\n  20. Licensee's use of linmath.h header for CPU functions for\r\n    GL vector/matrix operations from lunarG is subject to the\r\n    Apache License Version 2.0.\r\n\r\n  21. The DX12-CUDA sample uses the d3dx12.h header, which is\r\n    subject to the MIT license.\r\n\r\n  22. Components of the driver and compiler used for binary management, including \r\n    nvFatBin, nvcc, and cuobjdump, use the Zstandard library which is subject to\r\n\tthe following license:\r\n\r\n    BSD License\r\n\r\n    For Zstandard software\r\n\r\n    Copyright (c) Meta Platforms, Inc. and affiliates. All rights reserved.\r\n\r\n    Redistribution and use in source and binary forms, with or without modification,\r\n \tare permitted provided that the following conditions are met:\r\n\r\n        * Redistributions of source code must retain the above copyright notice, this\r\n      list of conditions and the following disclaimer.\r\n\r\n        * Redistributions in binary form must reproduce the above copyright notice,\r\n      this list of conditions and the following disclaimer in the documentation\r\n      and/or other materials provided with the distribution.\r\n\r\n        * Neither the name Facebook, nor Meta, nor the names of its contributors may\r\n      be used to endorse or promote products derived from this software without\r\n      specific prior written permission.\r\n\r\n    THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS \"AS IS\" AND\r\n    ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED\r\n    WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE\r\n    DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE FOR ANY\r\n    DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING,\r\n    BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA,\r\n    OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY,\r\n    WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)\r\n    ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY\r\n    OF SUCH DAMAGE.\r\n\r\n-----------------\r\n"},
                            new TextBlock { FontSize=16,Margin = new Microsoft.UI.Xaml.Thickness(0, 0, 0, 10),Text = "NVIDIA TensorRT-10.11.0.33" },
                            new TextBlock { FontSize=9,Margin = new Microsoft.UI.Xaml.Thickness(0, 0, 0, 20),TextWrapping=TextWrapping.Wrap ,Text = "Abstract\r\nThis document is the LICENSE AGREEMENT FOR NVIDIA SOFTWARE DEVELOPMENT KITS for NVIDIA TensorRT. This document contains specific license terms and conditions for NVIDIA TensorRT. By accepting this agreement, you agree to comply with all the terms and conditions applicable to the specific product(s) included herein.\r\n\r\nThis license agreement, including exhibits attached (\"Agreement”) is a legal agreement between you and NVIDIA Corporation (\"NVIDIA\") and governs your use of a NVIDIA software development kit (“SDK”).\r\n\r\nEach SDK has its own set of software and materials, but here is a description of the types of items that may be included in a SDK: source code, header files, APIs, data sets and assets (examples include images, textures, models, scenes, videos, native API input/output files), binary software, sample code, libraries, utility programs, programming code and documentation.\r\n\r\nThis Agreement can be accepted only by an adult of legal age of majority in the country in which the SDK is used.\r\n\r\nIf you are entering into this Agreement on behalf of a company or other legal entity, you represent that you have the legal authority to bind the entity to this Agreement, in which case “you” will mean the entity you represent.\r\n\r\nIf you don’t have the required age or authority to accept this Agreement, or if you don’t accept all the terms and conditions of this Agreement, do not download, install or use the SDK.\r\n\r\nYou agree to use the SDK only for purposes that are permitted by (a) this Agreement, and (b) any applicable law, regulation or generally accepted practices or guidelines in the relevant jurisdictions.\r\n\r\n1. License.\r\n1.1. Grant\r\nSubject to the terms of this Agreement, NVIDIA hereby grants you a non-exclusive, non-transferable license, without the right to sublicense (except as expressly provided in this Agreement) to:\r\nInstall and use the SDK,\r\nModify and create derivative works of sample source code delivered in the SDK, and\r\nDistribute those portions of the SDK that are identified in this Agreement as distributable, as incorporated in object code format into a software application that meets the distribution requirements indicated in this Agreement.\r\n1.2. Distribution Requirements\r\nThese are the distribution requirements for you to exercise the distribution grant:\r\nYour application must have material additional functionality, beyond the included portions of the SDK.\r\nThe distributable portions of the SDK shall only be accessed by your application.\r\nThe following notice shall be included in modifications and derivative works of sample source code distributed: “This software contains source code provided by NVIDIA Corporation.”\r\nUnless a developer tool is identified in this Agreement as distributable, it is delivered for your internal use only.\r\nThe terms under which you distribute your application must be consistent with the terms of this Agreement, including (without limitation) terms relating to the license grant and license restrictions and protection of NVIDIA’s intellectual property rights. Additionally, you agree that you will protect the privacy, security and legal rights of your application users.\r\nYou agree to notify NVIDIA in writing of any known or suspected distribution or use of the SDK not in compliance with the requirements of this Agreement, and to enforce the terms of your agreements with respect to distributed SDK.\r\n1.3. Authorized Users\r\nYou may allow employees and contractors of your entity or of your subsidiary(ies) to access and use the SDK from your secure network to perform work on your behalf.\r\nIf you are an academic institution you may allow users enrolled or employed by the academic institution to access and use the SDK from your secure network.\r\n\r\nYou are responsible for the compliance with the terms of this Agreement by your authorized users. If you become aware that your authorized users didn’t follow the terms of this Agreement, you agree to take reasonable steps to resolve the non-compliance and prevent new occurrences.\r\n\r\n1.4. Pre-Release SDK\r\nThe SDK versions identified as alpha, beta, preview or otherwise as pre-release, may not be fully functional, may contain errors or design flaws, and may have reduced or different security, privacy, accessibility, availability, and reliability standards relative to commercial versions of NVIDIA software and materials. Use of a pre-release SDK may result in unexpected results, loss of data, project delays or other unpredictable damage or loss.\r\nYou may use a pre-release SDK at your own risk, understanding that pre-release SDKs are not intended for use in production or business-critical systems.\r\n\r\nNVIDIA may choose not to make available a commercial version of any pre-release SDK. NVIDIA may also choose to abandon development and terminate the availability of a pre-release SDK at any time without liability.\r\n\r\n1.5. Updates\r\nNVIDIA may, at its option, make available patches, workarounds or other updates to this SDK. Unless the updates are provided with their separate governing terms, they are deemed part of the SDK licensed to you as provided in this Agreement.\r\nYou agree that the form and content of the SDK that NVIDIA provides may change without prior notice to you. While NVIDIA generally maintains compatibility between versions, NVIDIA may in some cases make changes that introduce incompatibilities in future versions of the SDK.\r\n\r\n1.6. Third Party Licenses\r\nThe SDK may come bundled with, or otherwise include or be distributed with, third party software licensed by a NVIDIA supplier and/or open source software provided under an open source license. Use of third party software is subject to the third-party license terms, or in the absence of third party terms, the terms of this Agreement. Copyright to third party software is held by the copyright holders indicated in the third-party software or license.\r\n1.7. Reservation of Rights\r\nNVIDIA reserves all rights, title and interest in and to the SDK not expressly granted to you under this Agreement.\r\n2. Limitations.\r\nThe following license limitations apply to your use of the SDK:\r\n\r\n2.1 You may not reverse engineer, decompile or disassemble, or remove copyright or other proprietary notices from any portion of the SDK or copies of the SDK.\r\n\r\n2.2 Except as expressly provided in this Agreement, you may not copy, sell, rent, sublicense, transfer, distribute, modify, or create derivative works of any portion of the SDK.\r\n\r\n2.3 Unless you have an agreement with NVIDIA for this purpose, you may not indicate that an application created with the SDK is sponsored or endorsed by NVIDIA.\r\n\r\n2.4 You may not bypass, disable, or circumvent any encryption, security, digital rights management or authentication mechanism in the SDK.\r\n\r\n2.5 You may not use the SDK in any manner that would cause it to become subject to an open source software license. As examples, licenses that require as a condition of use, modification, and/or distribution that the SDK be (i) disclosed or distributed in source code form; (ii) licensed for the purpose of making derivative works; or (iii) redistributable at no charge.\r\n\r\n2.6 You acknowledge that the SDK as delivered is not tested or certified by NVIDIA for use in connection with the design, construction, maintenance, and/or operation of any system where the use or failure of such system could result in a situation that threatens the safety of human life or results in catastrophic damages (each, a \"Critical Application\"). Examples of Critical Applications includes use of avionics, navigation, autonomous vehicle applications, ai solutions for automotive products, military, medical, life support or other life critical applications. NVIDIA shall not be liable to you or any third party, in whole or in part, for any claims or damages arising from such uses. You are solely responsible for ensuring that any product or service developed with the SDK as a whole includes sufficient features to comply with all applicable legal and regulatory standards and requirements.\r\n\r\n2.7 You agree to defend, indemnify and hold harmless NVIDIA and its affiliates, and their respective employees, contractors, agents, officers and directors, from and against any and all claims, damages, obligations, losses, liabilities, costs or debt, fines, restitutions and expenses (including but not limited to attorney’s fees and costs incident to establishing the right of indemnification) arising out of or related to products or services that use the SDK in or for Critical Applications, and for use of the SDK outside of the scope of this Agreement or not in compliance with its terms.\r\n\r\n3. Ownership.\r\n3.1 NVIDIA or its licensors hold all rights, title and interest in and to the SDK and its modifications and derivative works, including their respective intellectual property rights, subject to your rights under Section 3.2. This SDK may include software and materials from NVIDIA’s licensors, and these licensors are intended third party beneficiaries that may enforce this Agreement with respect to their intellectual property rights.\r\n\r\n3.2 You hold all rights, title and interest in and to your applications and your derivative works of the sample source code delivered in the SDK, including their respective intellectual property rights, subject to NVIDIA’s rights under section 3.1.\r\n\r\n3.3 You may, but don’t have to, provide to NVIDIA suggestions, feature requests or other feedback regarding the SDK, including possible enhancements or modifications to the SDK. For any feedback that you voluntarily provide, you hereby grant NVIDIA and its affiliates a perpetual, non-exclusive, worldwide, irrevocable license to use, reproduce, modify, license, sublicense (through multiple tiers of sublicensees), and distribute (through multiple tiers of distributors) it without the payment of any royalties or fees to you. NVIDIA will use feedback at its choice. NVIDIA is constantly looking for ways to improve its products, so you may send feedback to NVIDIA through the developer portal at https://developer.nvidia.com.\r\n\r\n4. No Warranties.\r\nTHE SDK IS PROVIDED BY NVIDIA \"ASIS\" AND \"WITH ALL FAULTS.\" TO THE MAXIMUM EXTENT PERMITTED BY LAW, NVIDIA AND ITS AFFILIATES EXPRESSLY DISCLAIM ALL WARRANTIES OF ANY KIND OR NATURE, WHETHER EXPRESS, IMPLIED OR STATUTORY, INCLUDING, BUT NOT LIMITED TO, ANY WARRANTIES OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE, TITLE, NON-INFRINGEMENT, OR THE ABSENCE OF ANY DEFECTS THEREIN, WHETHER LATENT OR PATENT. NO WARRANTY IS MADE ON THE BASIS OF TRADE USAGE, COURSE OF DEALING OR COURSE OF TRADE.\r\n5. Limitations of Liability.\r\nTO THE MAXIMUM EXTENT PERMITTED BY LAW, NVIDIA AND ITS AFFILIATES SHALL NOT BE LIABLE FOR ANY SPECIAL, INCIDENTAL, PUNITIVE OR CONSEQUENTIAL DAMAGES, OR ANY LOST PROFITS, LOSS OF USE, LOSS OF DATA OR LOSS OF GOODWILL, OR THE COSTS OF PROCURING SUBSTITUTE PRODUCTS, ARISING OUT OF OR IN CONNECTION WITH THIS AGREEMENT OR THE USE OR PERFORMANCE OF THE SDK, WHETHER SUCH LIABILITY ARISES FROM ANY CLAIM BASED UPON BREACH OF CONTRACT, BREACH OF WARRANTY, TORT (INCLUDING NEGLIGENCE), PRODUCT LIABILITY OR ANY OTHER CAUSE OF ACTION OR THEORY OF LIABILITY. IN NO EVENT WILL NVIDIA’S AND ITS AFFILIATES TOTAL CUMULATIVE LIABILITY UNDER OR ARISING OUT OF THIS AGREEMENT EXCEED US$10.00. THE NATURE OF THE LIABILITY OR THE NUMBER OF CLAIMS OR SUITS SHALL NOT ENLARGE OR EXTEND THIS LIMIT.\r\nThese exclusions and limitations of liability shall apply regardless if NVIDIA or its affiliates have been advised of the possibility of such damages, and regardless of whether a remedy fails its essential purpose. These exclusions and limitations of liability form an essential basis of the bargain between the parties, and, absent any of these exclusions or limitations of liability, the provisions of this Agreement, including, without limitation, the economic terms, would be substantially different.\r\n\r\n6. Termination.\r\n6.1 This Agreement will continue to apply until terminated by either you or NVIDIA as described below.\r\n\r\n6.2 If you want to terminate this Agreement, you may do so by stopping to use the SDK.\r\n\r\n6.3 NVIDIA may, at any time, terminate this Agreement if: (i) you fail to comply with any term of this Agreement and the non-compliance is not fixed within thirty (30) days following notice from NVIDIA (or immediately if you violate NVIDIA’s intellectual property rights); (ii) you commence or participate in any legal proceeding against NVIDIA with respect to the SDK; or (iii) NVIDIA decides to no longer provide the SDK in a country or, in NVIDIA’s sole discretion, the continued use of it is no longer commercially viable.\r\n\r\n6.4 Upon any termination of this Agreement, you agree to promptly discontinue use of the SDK and destroy all copies in your possession or control. Your prior distributions in accordance with this Agreement are not affected by the termination of this Agreement. Upon written request, you will certify in writing that you have complied with your commitments under this section. Upon any termination of this Agreement all provisions survive except for the licenses granted to you.\r\n\r\n7. General.\r\nIf you wish to assign this Agreement or your rights and obligations, including by merger, consolidation, dissolution or operation of law, contact NVIDIA to ask for permission. Any attempted assignment not approved by NVIDIA in writing shall be void and of no effect. NVIDIA may assign, delegate or transfer this Agreement and its rights and obligations, and if to a non-affiliate you will be notified.\r\nYou agree to cooperate with NVIDIA and provide reasonably requested information to verify your compliance with this Agreement.\r\n\r\nThis Agreement will be governed in all respects by the laws of the United States and of the State of Delaware as those laws are applied to contracts entered into and performed entirely within Delaware by Delaware residents, without regard to the conflicts of laws principles. The United Nations Convention on Contracts for the International Sale of Goods is specifically disclaimed. You agree to all terms of this Agreement in the English language.\r\n\r\nThe state or federal courts residing in Santa Clara County, California shall have exclusive jurisdiction over any dispute or claim arising out of this Agreement. Notwithstanding this, you agree that NVIDIA shall still be allowed to apply for injunctive remedies or an equivalent type of urgent legal relief in any jurisdiction.\r\n\r\nIf any court of competent jurisdiction determines that any provision of this Agreement is illegal, invalid or unenforceable, such provision will be construed as limited to the extent necessary to be consistent with and fully enforceable under the law and the remaining provisions will remain in full force and effect. Unless otherwise specified, remedies are cumulative.\r\n\r\nEach party acknowledges and agrees that the other is an independent contractor in the performance of this Agreement.\r\n\r\nThe SDK has been developed entirely at private expense and is “commercial items” consisting of “commercial computer software” and “commercial computer software documentation” provided with RESTRICTED RIGHTS. Use, duplication or disclosure by the U.S. Government or a U.S. Government subcontractor is subject to the restrictions in this Agreement pursuant to DFARS 227.7202-3(a) or as set forth in subparagraphs (b)(1) and (2) of the Commercial Computer Software - Restricted Rights clause at FAR 52.227-19, as applicable. Contractor/manufacturer is NVIDIA, 2788 San Tomas Expressway, Santa Clara, CA 95051.\r\n\r\nThe SDK is subject to United States export laws and regulations. You agree that you will not ship, transfer or export the SDK into any country, or use the SDK in any manner, prohibited by the United States Bureau of Industry and Security or economic sanctions regulations administered by the U.S. Department of Treasury’s Office of Foreign Assets Control (OFAC), or any applicable export laws, restrictions or regulations. These laws include restrictions on destinations, end users and end use. By accepting this Agreement, you confirm that you are not a resident or citizen of any country currently embargoed by the U.S. and that you are not otherwise prohibited from receiving the SDK.\r\n\r\nAny notice delivered by NVIDIA to you under this Agreement will be delivered via mail, email or fax. You agree that any notices that NVIDIA sends you electronically will satisfy any legal communication requirements. Please direct your legal notices or other correspondence to NVIDIA Corporation, 2788 San Tomas Expressway, Santa Clara, California 95051, United States of America, Attention: Legal Department.\r\n\r\nThis Agreement and any exhibits incorporated into this Agreement constitute the entire agreement of the parties with respect to the subject matter of this Agreement and supersede all prior negotiations or documentation exchanged between the parties relating to this SDK license. Any additional and/or conflicting terms on documents issued by you are null, void, and invalid. Any amendment or waiver under this Agreement shall be in writing and signed by representatives of both parties.\r\n\r\n(v. May 24, 2021)\r\n\r\n8. TensorRT SUPPLEMENT TO SOFTWARE LICENSE AGREEMENT FOR NVIDIA SOFTWARE DEVELOPMENT KITS\r\nThe terms in this supplement govern your use of the NVIDIA TensorRT SDK under the terms of your license agreement (“Agreement”) as modified by this supplement. Capitalized terms used but not defined below have the meaning assigned to them in the Agreement.\r\nThis supplement is an exhibit to the Agreement and is incorporated as an integral part of the Agreement. In the event of conflict between the terms in this supplement and the terms in the Agreement, the terms in this supplement govern.\r\n\r\n1 License Scope. The SDK is licensed for you to develop applications only for use in systems with NVIDIA GPUs.\r\n\r\n2 Distribution. The following portions of the SDK are distributable under the Agreement: the runtime files .so and .dll.\r\n\r\nIn addition to the rights above, for parties that are developing software intended solely for use on Jetson development kits or Jetson modules and running Linux for Tegra software the following shall apply: the SDK may be distributed in its entirety, as provided by NVIDIA and without separation of its components, for you and/or your licensees to create software development kits for use only on the Jetson platform and running Linux for Tegra software.\r\n\r\n3 Licensing. If the distribution terms in this Agreement are not suitable for your organization, or for any questions regarding this Agreement, please contact NVIDIA at nvidia-compute-license-questions@nvidia.com.\r\n\r\n(v. February 25, 2021)"},
                            new TextBlock { FontSize=16,Margin = new Microsoft.UI.Xaml.Thickness(0, 0, 0, 10),Text = "OpenCV 4.10" },
                            new TextBlock { FontSize=9,Margin = new Microsoft.UI.Xaml.Thickness(0, 0, 0, 20),TextWrapping=TextWrapping.Wrap ,Text = "\r\n                                 Apache License\r\n                           Version 2.0, January 2004\r\n                        http://www.apache.org/licenses/\r\n\r\n   TERMS AND CONDITIONS FOR USE, REPRODUCTION, AND DISTRIBUTION\r\n\r\n   1. Definitions.\r\n\r\n      \"License\" shall mean the terms and conditions for use, reproduction,\r\n      and distribution as defined by Sections 1 through 9 of this document.\r\n\r\n      \"Licensor\" shall mean the copyright owner or entity authorized by\r\n      the copyright owner that is granting the License.\r\n\r\n      \"Legal Entity\" shall mean the union of the acting entity and all\r\n      other entities that control, are controlled by, or are under common\r\n      control with that entity. For the purposes of this definition,\r\n      \"control\" means (i) the power, direct or indirect, to cause the\r\n      direction or management of such entity, whether by contract or\r\n      otherwise, or (ii) ownership of fifty percent (50%) or more of the\r\n      outstanding shares, or (iii) beneficial ownership of such entity.\r\n\r\n      \"You\" (or \"Your\") shall mean an individual or Legal Entity\r\n      exercising permissions granted by this License.\r\n\r\n      \"Source\" form shall mean the preferred form for making modifications,\r\n      including but not limited to software source code, documentation\r\n      source, and configuration files.\r\n\r\n      \"Object\" form shall mean any form resulting from mechanical\r\n      transformation or translation of a Source form, including but\r\n      not limited to compiled object code, generated documentation,\r\n      and conversions to other media types.\r\n\r\n      \"Work\" shall mean the work of authorship, whether in Source or\r\n      Object form, made available under the License, as indicated by a\r\n      copyright notice that is included in or attached to the work\r\n      (an example is provided in the Appendix below).\r\n\r\n      \"Derivative Works\" shall mean any work, whether in Source or Object\r\n      form, that is based on (or derived from) the Work and for which the\r\n      editorial revisions, annotations, elaborations, or other modifications\r\n      represent, as a whole, an original work of authorship. For the purposes\r\n      of this License, Derivative Works shall not include works that remain\r\n      separable from, or merely link (or bind by name) to the interfaces of,\r\n      the Work and Derivative Works thereof.\r\n\r\n      \"Contribution\" shall mean any work of authorship, including\r\n      the original version of the Work and any modifications or additions\r\n      to that Work or Derivative Works thereof, that is intentionally\r\n      submitted to Licensor for inclusion in the Work by the copyright owner\r\n      or by an individual or Legal Entity authorized to submit on behalf of\r\n      the copyright owner. For the purposes of this definition, \"submitted\"\r\n      means any form of electronic, verbal, or written communication sent\r\n      to the Licensor or its representatives, including but not limited to\r\n      communication on electronic mailing lists, source code control systems,\r\n      and issue tracking systems that are managed by, or on behalf of, the\r\n      Licensor for the purpose of discussing and improving the Work, but\r\n      excluding communication that is conspicuously marked or otherwise\r\n      designated in writing by the copyright owner as \"Not a Contribution.\"\r\n\r\n      \"Contributor\" shall mean Licensor and any individual or Legal Entity\r\n      on behalf of whom a Contribution has been received by Licensor and\r\n      subsequently incorporated within the Work.\r\n\r\n   2. Grant of Copyright License. Subject to the terms and conditions of\r\n      this License, each Contributor hereby grants to You a perpetual,\r\n      worldwide, non-exclusive, no-charge, royalty-free, irrevocable\r\n      copyright license to reproduce, prepare Derivative Works of,\r\n      publicly display, publicly perform, sublicense, and distribute the\r\n      Work and such Derivative Works in Source or Object form.\r\n\r\n   3. Grant of Patent License. Subject to the terms and conditions of\r\n      this License, each Contributor hereby grants to You a perpetual,\r\n      worldwide, non-exclusive, no-charge, royalty-free, irrevocable\r\n      (except as stated in this section) patent license to make, have made,\r\n      use, offer to sell, sell, import, and otherwise transfer the Work,\r\n      where such license applies only to those patent claims licensable\r\n      by such Contributor that are necessarily infringed by their\r\n      Contribution(s) alone or by combination of their Contribution(s)\r\n      with the Work to which such Contribution(s) was submitted. If You\r\n      institute patent litigation against any entity (including a\r\n      cross-claim or counterclaim in a lawsuit) alleging that the Work\r\n      or a Contribution incorporated within the Work constitutes direct\r\n      or contributory patent infringement, then any patent licenses\r\n      granted to You under this License for that Work shall terminate\r\n      as of the date such litigation is filed.\r\n\r\n   4. Redistribution. You may reproduce and distribute copies of the\r\n      Work or Derivative Works thereof in any medium, with or without\r\n      modifications, and in Source or Object form, provided that You\r\n      meet the following conditions:\r\n\r\n      (a) You must give any other recipients of the Work or\r\n          Derivative Works a copy of this License; and\r\n\r\n      (b) You must cause any modified files to carry prominent notices\r\n          stating that You changed the files; and\r\n\r\n      (c) You must retain, in the Source form of any Derivative Works\r\n          that You distribute, all copyright, patent, trademark, and\r\n          attribution notices from the Source form of the Work,\r\n          excluding those notices that do not pertain to any part of\r\n          the Derivative Works; and\r\n\r\n      (d) If the Work includes a \"NOTICE\" text file as part of its\r\n          distribution, then any Derivative Works that You distribute must\r\n          include a readable copy of the attribution notices contained\r\n          within such NOTICE file, excluding those notices that do not\r\n          pertain to any part of the Derivative Works, in at least one\r\n          of the following places: within a NOTICE text file distributed\r\n          as part of the Derivative Works; within the Source form or\r\n          documentation, if provided along with the Derivative Works; or,\r\n          within a display generated by the Derivative Works, if and\r\n          wherever such third-party notices normally appear. The contents\r\n          of the NOTICE file are for informational purposes only and\r\n          do not modify the License. You may add Your own attribution\r\n          notices within Derivative Works that You distribute, alongside\r\n          or as an addendum to the NOTICE text from the Work, provided\r\n          that such additional attribution notices cannot be construed\r\n          as modifying the License.\r\n\r\n      You may add Your own copyright statement to Your modifications and\r\n      may provide additional or different license terms and conditions\r\n      for use, reproduction, or distribution of Your modifications, or\r\n      for any such Derivative Works as a whole, provided Your use,\r\n      reproduction, and distribution of the Work otherwise complies with\r\n      the conditions stated in this License.\r\n\r\n   5. Submission of Contributions. Unless You explicitly state otherwise,\r\n      any Contribution intentionally submitted for inclusion in the Work\r\n      by You to the Licensor shall be under the terms and conditions of\r\n      this License, without any additional terms or conditions.\r\n      Notwithstanding the above, nothing herein shall supersede or modify\r\n      the terms of any separate license agreement you may have executed\r\n      with Licensor regarding such Contributions.\r\n\r\n   6. Trademarks. This License does not grant permission to use the trade\r\n      names, trademarks, service marks, or product names of the Licensor,\r\n      except as required for reasonable and customary use in describing the\r\n      origin of the Work and reproducing the content of the NOTICE file.\r\n\r\n   7. Disclaimer of Warranty. Unless required by applicable law or\r\n      agreed to in writing, Licensor provides the Work (and each\r\n      Contributor provides its Contributions) on an \"AS IS\" BASIS,\r\n      WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or\r\n      implied, including, without limitation, any warranties or conditions\r\n      of TITLE, NON-INFRINGEMENT, MERCHANTABILITY, or FITNESS FOR A\r\n      PARTICULAR PURPOSE. You are solely responsible for determining the\r\n      appropriateness of using or redistributing the Work and assume any\r\n      risks associated with Your exercise of permissions under this License.\r\n\r\n   8. Limitation of Liability. In no event and under no legal theory,\r\n      whether in tort (including negligence), contract, or otherwise,\r\n      unless required by applicable law (such as deliberate and grossly\r\n      negligent acts) or agreed to in writing, shall any Contributor be\r\n      liable to You for damages, including any direct, indirect, special,\r\n      incidental, or consequential damages of any character arising as a\r\n      result of this License or out of the use or inability to use the\r\n      Work (including but not limited to damages for loss of goodwill,\r\n      work stoppage, computer failure or malfunction, or any and all\r\n      other commercial damages or losses), even if such Contributor\r\n      has been advised of the possibility of such damages.\r\n\r\n   9. Accepting Warranty or Additional Liability. While redistributing\r\n      the Work or Derivative Works thereof, You may choose to offer,\r\n      and charge a fee for, acceptance of support, warranty, indemnity,\r\n      or other liability obligations and/or rights consistent with this\r\n      License. However, in accepting such obligations, You may act only\r\n      on Your own behalf and on Your sole responsibility, not on behalf\r\n      of any other Contributor, and only if You agree to indemnify,\r\n      defend, and hold each Contributor harmless for any liability\r\n      incurred by, or claims asserted against, such Contributor by reason\r\n      of your accepting any such warranty or additional liability.\r\n\r\n   END OF TERMS AND CONDITIONS\r\n\r\n   APPENDIX: How to apply the Apache License to your work.\r\n\r\n      To apply the Apache License to your work, attach the following\r\n      boilerplate notice, with the fields enclosed by brackets \"[]\"\r\n      replaced with your own identifying information. (Don't include\r\n      the brackets!)  The text should be enclosed in the appropriate\r\n      comment syntax for the file format. We also recommend that a\r\n      file or class name and description of purpose be included on the\r\n      same \"printed page\" as the copyright notice for easier\r\n      identification within third-party archives.\r\n\r\n   Copyright [yyyy] [name of copyright owner]\r\n\r\n   Licensed under the Apache License, Version 2.0 (the \"License\");\r\n   you may not use this file except in compliance with the License.\r\n   You may obtain a copy of the License at\r\n\r\n       http://www.apache.org/licenses/LICENSE-2.0\r\n\r\n   Unless required by applicable law or agreed to in writing, software\r\n   distributed under the License is distributed on an \"AS IS\" BASIS,\r\n   WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.\r\n   See the License for the specific language governing permissions and\r\n   limitations under the License."},
                            new TextBlock { FontSize=16,Margin = new Microsoft.UI.Xaml.Thickness(0, 0, 0, 10),Text = "FFmpeg 7.1" },
                            new TextBlock { FontSize=9,Margin = new Microsoft.UI.Xaml.Thickness(0, 0, 0, 20),TextWrapping=TextWrapping.Wrap ,Text = "                   GNU LESSER GENERAL PUBLIC LICENSE\r\n                       Version 3, 29 June 2007\r\n\r\n Copyright (C) 2007 Free Software Foundation, Inc. <http://fsf.org/>\r\n Everyone is permitted to copy and distribute verbatim copies\r\n of this license document, but changing it is not allowed.\r\n\r\n\r\n  This version of the GNU Lesser General Public License incorporates\r\nthe terms and conditions of version 3 of the GNU General Public\r\nLicense, supplemented by the additional permissions listed below.\r\n\r\n  0. Additional Definitions.\r\n\r\n  As used herein, \"this License\" refers to version 3 of the GNU Lesser\r\nGeneral Public License, and the \"GNU GPL\" refers to version 3 of the GNU\r\nGeneral Public License.\r\n\r\n  \"The Library\" refers to a covered work governed by this License,\r\nother than an Application or a Combined Work as defined below.\r\n\r\n  An \"Application\" is any work that makes use of an interface provided\r\nby the Library, but which is not otherwise based on the Library.\r\nDefining a subclass of a class defined by the Library is deemed a mode\r\nof using an interface provided by the Library.\r\n\r\n  A \"Combined Work\" is a work produced by combining or linking an\r\nApplication with the Library.  The particular version of the Library\r\nwith which the Combined Work was made is also called the \"Linked\r\nVersion\".\r\n\r\n  The \"Minimal Corresponding Source\" for a Combined Work means the\r\nCorresponding Source for the Combined Work, excluding any source code\r\nfor portions of the Combined Work that, considered in isolation, are\r\nbased on the Application, and not on the Linked Version.\r\n\r\n  The \"Corresponding Application Code\" for a Combined Work means the\r\nobject code and/or source code for the Application, including any data\r\nand utility programs needed for reproducing the Combined Work from the\r\nApplication, but excluding the System Libraries of the Combined Work.\r\n\r\n  1. Exception to Section 3 of the GNU GPL.\r\n\r\n  You may convey a covered work under sections 3 and 4 of this License\r\nwithout being bound by section 3 of the GNU GPL.\r\n\r\n  2. Conveying Modified Versions.\r\n\r\n  If you modify a copy of the Library, and, in your modifications, a\r\nfacility refers to a function or data to be supplied by an Application\r\nthat uses the facility (other than as an argument passed when the\r\nfacility is invoked), then you may convey a copy of the modified\r\nversion:\r\n\r\n   a) under this License, provided that you make a good faith effort to\r\n   ensure that, in the event an Application does not supply the\r\n   function or data, the facility still operates, and performs\r\n   whatever part of its purpose remains meaningful, or\r\n\r\n   b) under the GNU GPL, with none of the additional permissions of\r\n   this License applicable to that copy.\r\n\r\n  3. Object Code Incorporating Material from Library Header Files.\r\n\r\n  The object code form of an Application may incorporate material from\r\na header file that is part of the Library.  You may convey such object\r\ncode under terms of your choice, provided that, if the incorporated\r\nmaterial is not limited to numerical parameters, data structure\r\nlayouts and accessors, or small macros, inline functions and templates\r\n(ten or fewer lines in length), you do both of the following:\r\n\r\n   a) Give prominent notice with each copy of the object code that the\r\n   Library is used in it and that the Library and its use are\r\n   covered by this License.\r\n\r\n   b) Accompany the object code with a copy of the GNU GPL and this license\r\n   document.\r\n\r\n  4. Combined Works.\r\n\r\n  You may convey a Combined Work under terms of your choice that,\r\ntaken together, effectively do not restrict modification of the\r\nportions of the Library contained in the Combined Work and reverse\r\nengineering for debugging such modifications, if you also do each of\r\nthe following:\r\n\r\n   a) Give prominent notice with each copy of the Combined Work that\r\n   the Library is used in it and that the Library and its use are\r\n   covered by this License.\r\n\r\n   b) Accompany the Combined Work with a copy of the GNU GPL and this license\r\n   document.\r\n\r\n   c) For a Combined Work that displays copyright notices during\r\n   execution, include the copyright notice for the Library among\r\n   these notices, as well as a reference directing the user to the\r\n   copies of the GNU GPL and this license document.\r\n\r\n   d) Do one of the following:\r\n\r\n       0) Convey the Minimal Corresponding Source under the terms of this\r\n       License, and the Corresponding Application Code in a form\r\n       suitable for, and under terms that permit, the user to\r\n       recombine or relink the Application with a modified version of\r\n       the Linked Version to produce a modified Combined Work, in the\r\n       manner specified by section 6 of the GNU GPL for conveying\r\n       Corresponding Source.\r\n\r\n       1) Use a suitable shared library mechanism for linking with the\r\n       Library.  A suitable mechanism is one that (a) uses at run time\r\n       a copy of the Library already present on the user's computer\r\n       system, and (b) will operate properly with a modified version\r\n       of the Library that is interface-compatible with the Linked\r\n       Version.\r\n\r\n   e) Provide Installation Information, but only if you would otherwise\r\n   be required to provide such information under section 6 of the\r\n   GNU GPL, and only to the extent that such information is\r\n   necessary to install and execute a modified version of the\r\n   Combined Work produced by recombining or relinking the\r\n   Application with a modified version of the Linked Version. (If\r\n   you use option 4d0, the Installation Information must accompany\r\n   the Minimal Corresponding Source and Corresponding Application\r\n   Code. If you use option 4d1, you must provide the Installation\r\n   Information in the manner specified by section 6 of the GNU GPL\r\n   for conveying Corresponding Source.)\r\n\r\n  5. Combined Libraries.\r\n\r\n  You may place library facilities that are a work based on the\r\nLibrary side by side in a single library together with other library\r\nfacilities that are not Applications and are not covered by this\r\nLicense, and convey such a combined library under terms of your\r\nchoice, if you do both of the following:\r\n\r\n   a) Accompany the combined library with a copy of the same work based\r\n   on the Library, uncombined with any other library facilities,\r\n   conveyed under the terms of this License.\r\n\r\n   b) Give prominent notice with the combined library that part of it\r\n   is a work based on the Library, and explaining where to find the\r\n   accompanying uncombined form of the same work.\r\n\r\n  6. Revised Versions of the GNU Lesser General Public License.\r\n\r\n  The Free Software Foundation may publish revised and/or new versions\r\nof the GNU Lesser General Public License from time to time. Such new\r\nversions will be similar in spirit to the present version, but may\r\ndiffer in detail to address new problems or concerns.\r\n\r\n  Each version is given a distinguishing version number. If the\r\nLibrary as you received it specifies that a certain numbered version\r\nof the GNU Lesser General Public License \"or any later version\"\r\napplies to it, you have the option of following the terms and\r\nconditions either of that published version or of any later version\r\npublished by the Free Software Foundation. If the Library as you\r\nreceived it does not specify a version number of the GNU Lesser\r\nGeneral Public License, you may choose any version of the GNU Lesser\r\nGeneral Public License ever published by the Free Software Foundation.\r\n\r\n  If the Library as you received it specifies that a proxy can decide\r\nwhether future versions of the GNU Lesser General Public License shall\r\napply, that proxy's public statement of acceptance of any version is\r\npermanent authorization for you to choose that version for the\r\nLibrary.\r\n" },
                            new TextBlock { FontSize=16,Margin = new Microsoft.UI.Xaml.Thickness(0, 0, 0, 10),Text = "Ultralytics 8.3.36" },
                            new TextBlock { FontSize=9,Margin = new Microsoft.UI.Xaml.Thickness(0, 0, 0, 20),TextWrapping=TextWrapping.Wrap ,Text = "AGPL 3.0 Software License\r\nLast updated:\r\n\r\nVersion 3, 19 November 2007\r\n\r\n‍\r\n\r\nCopyright © 2007 Free Software Foundation, Inc. <https://fsf.org/>\r\nEveryone is permitted to copy and distribute verbatim copies of this license document, but changing it is not allowed.\r\n\r\nPreamble\r\nThe GNU Affero General Public License is a free, copyleft license for software and other kinds of works, specifically designed to ensure cooperation with the community in the case of network server software.\r\n\r\nThe licenses for most software and other practical works are designed to take away your freedom to share and change the works. By contrast, our General Public Licenses are intended to guarantee your freedom to share and change all versions of a program--to make sure it remains free software for all its users.\r\n\r\nWhen we speak of free software, we are referring to freedom, not price. Our General Public Licenses are designed to make sure that you have the freedom to distribute copies of free software (and charge for them if you wish), that you receive source code or can get it if you want it, that you can change the software or use pieces of it in new free programs, and that you know you can do these things.\r\n\r\nDevelopers that use our General Public Licenses protect your rights with two steps: (1) assert copyright on the software, and (2) offer you this License which gives you legal permission to copy, distribute and/or modify the software.\r\n\r\nA secondary benefit of defending all users' freedom is that improvements made in alternate versions of the program, if they receive widespread use, become available for other developers to incorporate. Many developers of free software are heartened and encouraged by the resulting cooperation. However, in the case of software used on network servers, this result may fail to come about. The GNU General Public License permits making a modified version and letting the public access it on a server without ever releasing its source code to the public.\r\n\r\nThe GNU Affero General Public License is designed specifically to ensure that, in such cases, the modified source code becomes available to the community. It requires the operator of a network server to provide the source code of the modified version running there to the users of that server. Therefore, public use of a modified version, on a publicly accessible server, gives the public access to the source code of the modified version.\r\n\r\nAn older license, called the Affero General Public License and published by Affero, was designed to accomplish similar goals. This is a different license, not a version of the Affero GPL, but Affero has released a new version of the Affero GPL which permits relicensing under this license.\r\n\r\nThe precise terms and conditions for copying, distribution and modification follow.\r\n\r\nTERMS AND CONDITIONS\r\n0. Definitions.\r\n\"This License\" refers to version 3 of the GNU Affero General Public License.\r\n\r\n\"Copyright\" also means copyright-like laws that apply to other kinds of works, such as semiconductor masks.\r\n\r\n\"The Program\" refers to any copyrightable work licensed under this License. Each licensee is addressed as \"you\". \"Licensees\" and \"recipients\" may be individuals or organizations.\r\n\r\nTo \"modify\" a work means to copy from or adapt all or part of the work in a fashion requiring copyright permission, other than the making of an exact copy. The resulting work is called a \"modified version\" of the earlier work or a work \"based on\" the earlier work.\r\n\r\nA \"covered work\" means either the unmodified Program or a work based on the Program.\r\n\r\nTo \"propagate\" a work means to do anything with it that, without permission, would make you directly or secondarily liable for infringement under applicable copyright law, except executing it on a computer or modifying a private copy. Propagation includes copying, distribution (with or without modification), making available to the public, and in some countries other activities as well.\r\n\r\nTo \"convey\" a work means any kind of propagation that enables other parties to make or receive copies. Mere interaction with a user through a computer network, with no transfer of a copy, is not conveying.\r\n\r\nAn interactive user interface displays \"Appropriate Legal Notices\" to the extent that it includes a convenient and prominently visible feature that (1) displays an appropriate copyright notice, and (2) tells the user that there is no warranty for the work (except to the extent that warranties are provided), that licensees may convey the work under this License, and how to view a copy of this License. If the interface presents a list of user commands or options, such as a menu, a prominent item in the list meets this criterion.\r\n\r\n1. Source Code.\r\nThe \"source code\" for a work means the preferred form of the work for making modifications to it. \"Object code\" means any non-source form of a work.\r\n\r\nA \"Standard Interface\" means an interface that either is an official standard defined by a recognized standards body, or, in the case of interfaces specified for a particular programming language, one that is widely used among developers working in that language.\r\n\r\nThe \"System Libraries\" of an executable work include anything, other than the work as a whole, that (a) is included in the normal form of packaging a Major Component, but which is not part of that Major Component, and (b) serves only to enable use of the work with that Major Component, or to implement a Standard Interface for which an implementation is available to the public in source code form. A \"Major Component\", in this context, means a major essential component (kernel, window system, and so on) of the specific operating system (if any) on which the executable work runs, or a compiler used to produce the work, or an object code interpreter used to run it.\r\n\r\nThe \"Corresponding Source\" for a work in object code form means all the source code needed to generate, install, and (for an executable work) run the object code and to modify the work, including scripts to control those activities. However, it does not include the work's System Libraries, or general-purpose tools or generally available free programs which are used unmodified in performing those activities but which are not part of the work. For example, Corresponding Source includes interface definition files associated with source files for the work, and the source code for shared libraries and dynamically linked subprograms that the work is specifically designed to require, such as by intimate data communication or control flow between those subprograms and other parts of the work.\r\n\r\nThe Corresponding Source need not include anything that users can regenerate automatically from other parts of the Corresponding Source.\r\n\r\nThe Corresponding Source for a work in source code form is that same work.\r\n\r\n2. Basic Permissions.\r\nAll rights granted under this License are granted for the term of copyright on the Program, and are irrevocable provided the stated conditions are met. This License explicitly affirms your unlimited permission to run the unmodified Program. The output from running a covered work is covered by this License only if the output, given its content, constitutes a covered work. This License acknowledges your rights of fair use or other equivalent, as provided by copyright law.\r\n\r\nYou may make, run and propagate covered works that you do not convey, without conditions so long as your license otherwise remains in force. You may convey covered works to others for the sole purpose of having them make modifications exclusively for you, or provide you with facilities for running those works, provided that you comply with the terms of this License in conveying all material for which you do not control copyright. Those thus making or running the covered works for you must do so exclusively on your behalf, under your direction and control, on terms that prohibit them from making any copies of your copyrighted material outside their relationship with you.\r\n\r\nConveying under any other circumstances is permitted solely under the conditions stated below. Sublicensing is not allowed; section 10 makes it unnecessary.\r\n\r\n3. Protecting Users' Legal Rights From Anti-Circumvention Law.\r\nNo covered work shall be deemed part of an effective technological measure under any applicable law fulfilling obligations under article 11 of the WIPO copyright treaty adopted on 20 December 1996, or similar laws prohibiting or restricting circumvention of such measures.\r\n\r\nWhen you convey a covered work, you waive any legal power to forbid circumvention of technological measures to the extent such circumvention is effected by exercising rights under this License with respect to the covered work, and you disclaim any intention to limit operation or modification of the work as a means of enforcing, against the work's users, your or third parties' legal rights to forbid circumvention of technological measures.\r\n\r\n4. Conveying Verbatim Copies.\r\nYou may convey verbatim copies of the Program's source code as you receive it, in any medium, provided that you conspicuously and appropriately publish on each copy an appropriate copyright notice; keep intact all notices stating that this License and any non-permissive terms added in accord with section 7 apply to the code; keep intact all notices of the absence of any warranty; and give all recipients a copy of this License along with the Program.\r\n\r\nYou may charge any price or no price for each copy that you convey, and you may offer support or warranty protection for a fee.\r\n\r\n5. Conveying Modified Source Versions.\r\nYou may convey a work based on the Program, or the modifications to produce it from the Program, in the form of source code under the terms of section 4, provided that you also meet all of these conditions:\r\n\r\na) The work must carry prominent notices stating that you modified it, and giving a relevant date.\r\nb) The work must carry prominent notices stating that it is released under this License and any conditions added under section 7. This requirement modifies the requirement in section 4 to \"keep intact all notices\".\r\nc) You must license the entire work, as a whole, under this License to anyone who comes into possession of a copy. This License will therefore apply, along with any applicable section 7 additional terms, to the whole of the work, and all its parts, regardless of how they are packaged. This License gives no permission to license the work in any other way, but it does not invalidate such permission if you have separately received it.\r\nd) If the work has interactive user interfaces, each must display Appropriate Legal Notices; however, if the Program has interactive interfaces that do not display Appropriate Legal Notices, your work need not make them do so.\r\nA compilation of a covered work with other separate and independent works, which are not by their nature extensions of the covered work, and which are not combined with it such as to form a larger program, in or on a volume of a storage or distribution medium, is called an \"aggregate\" if the compilation and its resulting copyright are not used to limit the access or legal rights of the compilation's users beyond what the individual works permit. Inclusion of a covered work in an aggregate does not cause this License to apply to the other parts of the aggregate.\r\n\r\n6. Conveying Non-Source Forms.\r\nYou may convey a covered work in object code form under the terms of sections 4 and 5, provided that you also convey the machine-readable Corresponding Source under the terms of this License, in one of these ways:\r\n\r\na) Convey the object code in, or embodied in, a physical product (including a physical distribution medium), accompanied by the Corresponding Source fixed on a durable physical medium customarily used for software interchange.\r\nb) Convey the object code in, or embodied in, a physical product (including a physical distribution medium), accompanied by a written offer, valid for at least three years and valid for as long as you offer spare parts or customer support for that product model, to give anyone who possesses the object code either (1) a copy of the Corresponding Source for all the software in the product that is covered by this License, on a durable physical medium customarily used for software interchange, for a price no more than your reasonable cost of physically performing this conveying of source, or (2) access to copy the Corresponding Source from a network server at no charge.\r\nc) Convey individual copies of the object code with a copy of the written offer to provide the Corresponding Source. This alternative is allowed only occasionally and noncommercially, and only if you received the object code with such an offer, in accord with subsection 6b.\r\nd) Convey the object code by offering access from a designated place (gratis or for a charge), and offer equivalent access to the Corresponding Source in the same way through the same place at no further charge. You need not require recipients to copy the Corresponding Source along with the object code. If the place to copy the object code is a network server, the Corresponding Source may be on a different server (operated by you or a third party) that supports equivalent copying facilities, provided you maintain clear directions next to the object code saying where to find the Corresponding Source. Regardless of what server hosts the Corresponding Source, you remain obligated to ensure that it is available for as long as needed to satisfy these requirements.\r\ne) Convey the object code using peer-to-peer transmission, provided you inform other peers where the object code and Corresponding Source of the work are being offered to the general public at no charge under subsection 6d.\r\nA separable portion of the object code, whose source code is excluded from the Corresponding Source as a System Library, need not be included in conveying the object code work.\r\n\r\nA \"User Product\" is either (1) a \"consumer product\", which means any tangible personal property which is normally used for personal, family, or household purposes, or (2) anything designed or sold for incorporation into a dwelling. In determining whether a product is a consumer product, doubtful cases shall be resolved in favor of coverage. For a particular product received by a particular user, \"normally used\" refers to a typical or common use of that class of product, regardless of the status of the particular user or of the way in which the particular user actually uses, or expects or is expected to use, the product. A product is a consumer product regardless of whether the product has substantial commercial, industrial or non-consumer uses, unless such uses represent the only significant mode of use of the product.\r\n\r\n\"Installation Information\" for a User Product means any methods, procedures, authorization keys, or other information required to install and execute modified versions of a covered work in that User Product from a modified version of its Corresponding Source. The information must suffice to ensure that the continued functioning of the modified object code is in no case prevented or interfered with solely because modification has been made.\r\n\r\nIf you convey an object code work under this section in, or with, or specifically for use in, a User Product, and the conveying occurs as part of a transaction in which the right of possession and use of the User Product is transferred to the recipient in perpetuity or for a fixed term (regardless of how the transaction is characterized), the Corresponding Source conveyed under this section must be accompanied by the Installation Information. But this requirement does not apply if neither you nor any third party retains the ability to install modified object code on the User Product (for example, the work has been installed in ROM).\r\n\r\nThe requirement to provide Installation Information does not include a requirement to continue to provide support service, warranty, or updates for a work that has been modified or installed by the recipient, or for the User Product in which it has been modified or installed. Access to a network may be denied when the modification itself materially and adversely affects the operation of the network or violates the rules and protocols for communication across the network.\r\n\r\nCorresponding Source conveyed, and Installation Information provided, in accord with this section must be in a format that is publicly documented (and with an implementation available to the public in source code form), and must require no special password or key for unpacking, reading or copying.\r\n\r\n7. Additional Terms.\r\n\"Additional permissions\" are terms that supplement the terms of this License by making exceptions from one or more of its conditions. Additional permissions that are applicable to the entire Program shall be treated as though they were included in this License, to the extent that they are valid under applicable law. If additional permissions apply only to part of the Program, that part may be used separately under those permissions, but the entire Program remains governed by this License without regard to the additional permissions.\r\n\r\nWhen you convey a copy of a covered work, you may at your option remove any additional permissions from that copy, or from any part of it. (Additional permissions may be written to require their own removal in certain cases when you modify the work.) You may place additional permissions on material, added by you to a covered work, for which you have or can give appropriate copyright permission.\r\n\r\nNotwithstanding any other provision of this License, for material you add to a covered work, you may (if authorized by the copyright holders of that material) supplement the terms of this License with terms:\r\n\r\na) Disclaiming warranty or limiting liability differently from the terms of sections 15 and 16 of this License; or\r\nb) Requiring preservation of specified reasonable legal notices or author attributions in that material or in the Appropriate Legal Notices displayed by works containing it; or\r\nc) Prohibiting misrepresentation of the origin of that material, or requiring that modified versions of such material be marked in reasonable ways as different from the original version; or\r\nd) Limiting the use for publicity purposes of names of licensors or authors of the material; or\r\ne) Declining to grant rights under trademark law for use of some trade names, trademarks, or service marks; or\r\nf) Requiring indemnification of licensors and authors of that material by anyone who conveys the material (or modified versions of it) with contractual assumptions of liability to the recipient, for any liability that these contractual assumptions directly impose on those licensors and authors.\r\nAll other non-permissive additional terms are considered \"further restrictions\" within the meaning of section 10. If the Program as you received it, or any part of it, contains a notice stating that it is governed by this License along with a term that is a further restriction, you may remove that term. If a license document contains a further restriction but permits relicensing or conveying under this License, you may add to a covered work material governed by the terms of that license document, provided that the further restriction does not survive such relicensing or conveying.\r\n\r\nIf you add terms to a covered work in accord with this section, you must place, in the relevant source files, a statement of the additional terms that apply to those files, or a notice indicating where to find the applicable terms.\r\n\r\nAdditional terms, permissive or non-permissive, may be stated in the form of a separately written license, or stated as exceptions; the above requirements apply either way.\r\n\r\n8. Termination.\r\nYou may not propagate or modify a covered work except as expressly provided under this License. Any attempt otherwise to propagate or modify it is void, and will automatically terminate your rights under this License (including any patent licenses granted under the third paragraph of section 11).\r\n\r\nHowever, if you cease all violation of this License, then your license from a particular copyright holder is reinstated (a) provisionally, unless and until the copyright holder explicitly and finally terminates your license, and (b) permanently, if the copyright holder fails to notify you of the violation by some reasonable means prior to 60 days after the cessation.\r\n\r\nMoreover, your license from a particular copyright holder is reinstated permanently if the copyright holder notifies you of the violation by some reasonable means, this is the first time you have received notice of violation of this License (for any work) from that copyright holder, and you cure the violation prior to 30 days after your receipt of the notice.\r\n\r\nTermination of your rights under this section does not terminate the licenses of parties who have received copies or rights from you under this License. If your rights have been terminated and not permanently reinstated, you do not qualify to receive new licenses for the same material under section 10.\r\n\r\n9. Acceptance Not Required for Having Copies.\r\nYou are not required to accept this License in order to receive or run a copy of the Program. Ancillary propagation of a covered work occurring solely as a consequence of using peer-to-peer transmission to receive a copy likewise does not require acceptance. However, nothing other than this License grants you permission to propagate or modify any covered work. These actions infringe copyright if you do not accept this License. Therefore, by modifying or propagating a covered work, you indicate your acceptance of this License to do so.\r\n\r\n10. Automatic Licensing of Downstream Recipients.\r\nEach time you convey a covered work, the recipient automatically receives a license from the original licensors, to run, modify and propagate that work, subject to this License. You are not responsible for enforcing compliance by third parties with this License.\r\n\r\nAn \"entity transaction\" is a transaction transferring control of an organization, or substantially all assets of one, or subdividing an organization, or merging organizations. If propagation of a covered work results from an entity transaction, each party to that transaction who receives a copy of the work also receives whatever licenses to the work the party's predecessor in interest had or could give under the previous paragraph, plus a right to possession of the Corresponding Source of the work from the predecessor in interest, if the predecessor has it or can get it with reasonable efforts.\r\n\r\nYou may not impose any further restrictions on the exercise of the rights granted or affirmed under this License. For example, you may not impose a license fee, royalty, or other charge for exercise of rights granted under this License, and you may not initiate litigation (including a cross-claim or counterclaim in a lawsuit) alleging that any patent claim is infringed by making, using, selling, offering for sale, or importing the Program or any portion of it.\r\n\r\n11. Patents.\r\nA \"contributor\" is a copyright holder who authorizes use under this License of the Program or a work on which the Program is based. The work thus licensed is called the contributor's \"contributor version\".\r\n\r\nA contributor's \"essential patent claims\" are all patent claims owned or controlled by the contributor, whether already acquired or hereafter acquired, that would be infringed by some manner, permitted by this License, of making, using, or selling its contributor version, but do not include claims that would be infringed only as a consequence of further modification of the contributor version. For purposes of this definition, \"control\" includes the right to grant patent sublicenses in a manner consistent with the requirements of this License.\r\n\r\nEach contributor grants you a non-exclusive, worldwide, royalty-free patent license under the contributor's essential patent claims, to make, use, sell, offer for sale, import and otherwise run, modify and propagate the contents of its contributor version.\r\n\r\nIn the following three paragraphs, a \"patent license\" is any express agreement or commitment, however denominated, not to enforce a patent (such as an express permission to practice a patent or covenant not to sue for patent infringement). To \"grant\" such a patent license to a party means to make such an agreement or commitment not to enforce a patent against the party.\r\n\r\nIf you convey a covered work, knowingly relying on a patent license, and the Corresponding Source of the work is not available for anyone to copy, free of charge and under the terms of this License, through a publicly available network server or other readily accessible means, then you must either (1) cause the Corresponding Source to be so available, or (2) arrange to deprive yourself of the benefit of the patent license for this particular work, or (3) arrange, in a manner consistent with the requirements of this License, to extend the patent license to downstream recipients. \"Knowingly relying\" means you have actual knowledge that, but for the patent license, your conveying the covered work in a country, or your recipient's use of the covered work in a country, would infringe one or more identifiable patents in that country that you have reason to believe are valid.\r\n\r\nIf, pursuant to or in connection with a single transaction or arrangement, you convey, or propagate by procuring conveyance of, a covered work, and grant a patent license to some of the parties receiving the covered work authorizing them to use, propagate, modify or convey a specific copy of the covered work, then the patent license you grant is automatically extended to all recipients of the covered work and works based on it.\r\n\r\nA patent license is \"discriminatory\" if it does not include within the scope of its coverage, prohibits the exercise of, or is conditioned on the non-exercise of one or more of the rights that are specifically granted under this License. You may not convey a covered work if you are a party to an arrangement with a third party that is in the business of distributing software, under which you make payment to the third party based on the extent of your activity of conveying the work, and under which the third party grants, to any of the parties who would receive the covered work from you, a discriminatory patent license (a) in connection with copies of the covered work conveyed by you (or copies made from those copies), or (b) primarily for and in connection with specific products or compilations that contain the covered work, unless you entered into that arrangement, or that patent license was granted, prior to 28 March 2007.\r\n\r\nNothing in this License shall be construed as excluding or limiting any implied license or other defenses to infringement that may otherwise be available to you under applicable patent law.\r\n\r\n12. No Surrender of Others' Freedom.\r\nIf conditions are imposed on you (whether by court order, agreement or otherwise) that contradict the conditions of this License, they do not excuse you from the conditions of this License. If you cannot convey a covered work so as to satisfy simultaneously your obligations under this License and any other pertinent obligations, then as a consequence you may not convey it at all. For example, if you agree to terms that obligate you to collect a royalty for further conveying from those to whom you convey the Program, the only way you could satisfy both those terms and this License would be to refrain entirely from conveying the Program.\r\n\r\n13. Remote Network Interaction; Use with the GNU General Public License.\r\nNotwithstanding any other provision of this License, if you modify the Program, your modified version must prominently offer all users interacting with it remotely through a computer network (if your version supports such interaction) an opportunity to receive the Corresponding Source of your version by providing access to the Corresponding Source from a network server at no charge, through some standard or customary means of facilitating copying of software. This Corresponding Source shall include the Corresponding Source for any work covered by version 3 of the GNU General Public License that is incorporated pursuant to the following paragraph.\r\n\r\nNotwithstanding any other provision of this License, you have permission to link or combine any covered work with a work licensed under version 3 of the GNU General Public License into a single combined work, and to convey the resulting work. The terms of this License will continue to apply to the part which is the covered work, but the work with which it is combined will remain governed by version 3 of the GNU General Public License.\r\n\r\n14. Revised Versions of this License.\r\nThe Free Software Foundation may publish revised and/or new versions of the GNU Affero General Public License from time to time. Such new versions will be similar in spirit to the present version, but may differ in detail to address new problems or concerns.\r\n\r\nEach version is given a distinguishing version number. If the Program specifies that a certain numbered version of the GNU Affero General Public License \"or any later version\" applies to it, you have the option of following the terms and conditions either of that numbered version or of any later version published by the Free Software Foundation. If the Program does not specify a version number of the GNU Affero General Public License, you may choose any version ever published by the Free Software Foundation.\r\n\r\nIf the Program specifies that a proxy can decide which future versions of the GNU Affero General Public License can be used, that proxy's public statement of acceptance of a version permanently authorizes you to choose that version for the Program.\r\n\r\nLater license versions may give you additional or different permissions. However, no additional obligations are imposed on any author or copyright holder as a result of your choosing to follow a later version.\r\n\r\n15. Disclaimer of Warranty.\r\nTHERE IS NO WARRANTY FOR THE PROGRAM, TO THE EXTENT PERMITTED BY APPLICABLE LAW. EXCEPT WHEN OTHERWISE STATED IN WRITING THE COPYRIGHT HOLDERS AND/OR OTHER PARTIES PROVIDE THE PROGRAM \"AS IS\" WITHOUT WARRANTY OF ANY KIND, EITHER EXPRESSED OR IMPLIED, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE. THE ENTIRE RISK AS TO THE QUALITY AND PERFORMANCE OF THE PROGRAM IS WITH YOU. SHOULD THE PROGRAM PROVE DEFECTIVE, YOU ASSUME THE COST OF ALL NECESSARY SERVICING, REPAIR OR CORRECTION.\r\n\r\n16. Limitation of Liability.\r\nIN NO EVENT UNLESS REQUIRED BY APPLICABLE LAW OR AGREED TO IN WRITING WILL ANY COPYRIGHT HOLDER, OR ANY OTHER PARTY WHO MODIFIES AND/OR CONVEYS THE PROGRAM AS PERMITTED ABOVE, BE LIABLE TO YOU FOR DAMAGES, INCLUDING ANY GENERAL, SPECIAL, INCIDENTAL OR CONSEQUENTIAL DAMAGES ARISING OUT OF THE USE OR INABILITY TO USE THE PROGRAM (INCLUDING BUT NOT LIMITED TO LOSS OF DATA OR DATA BEING RENDERED INACCURATE OR LOSSES SUSTAINED BY YOU OR THIRD PARTIES OR A FAILURE OF THE PROGRAM TO OPERATE WITH ANY OTHER PROGRAMS), EVEN IF SUCH HOLDER OR OTHER PARTY HAS BEEN ADVISED OF THE POSSIBILITY OF SUCH DAMAGES.\r\n\r\n17. Interpretation of Sections 15 and 16.\r\nIf the disclaimer of warranty and limitation of liability provided above cannot be given local legal effect according to their terms, reviewing courts shall apply local law that most closely approximates an absolute waiver of all civil liability in connection with the Program, unless a warranty or assumption of liability accompanies a copy of the Program in return for a fee.\r\n\r\nEND OF TERMS AND CONDITIONS\r\n\r\n‍\r\n\r\nHow to Apply These Terms to Your New Programs\r\nIf you develop a new program, and you want it to be of the greatest possible use to the public, the best way to achieve this is to make it free software which everyone can redistribute and change under these terms.\r\n\r\nTo do so, attach the following notices to the program. It is safest to attach them to the start of each source file to most effectively state the exclusion of warranty; and each file should have at least the \"copyright\" line and a pointer to where the full notice is found.\r\n\r\n   <one line to give the program's name and a brief idea of what it does.>    Copyright (C) <year>  <name of author>    This program is free software: you can redistribute it and/or modify    it under the terms of the GNU Affero General Public License as    published by the Free Software Foundation, either version 3 of the    License, or (at your option) any later version.    This program is distributed in the hope that it will be useful,    but WITHOUT ANY WARRANTY; without even the implied warranty of    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the    GNU Affero General Public License for more details.    You should have received a copy of the GNU Affero General Public License    along with this program.  If not, see <https://www.gnu.org/licenses/>.\r\nAlso add information on how to contact you by electronic and paper mail.\r\n\r\nIf your software can interact with users remotely through a computer network, you should also make sure that it provides a way for users to get its source. For example, if your program is a web application, its interface could display a \"Source\" link that leads users to an archive of the code. There are many ways you could offer source, and different solutions will be better for different programs; see section 13 for the specific requirements.\r\n\r\nYou should also get your employer (if you work as a programmer) or school, if any, to sign a \"copyright disclaimer\" for the program, if necessary. For more information on this, and how to apply and follow the GNU AGPL, see <https://www.gnu.org/licenses/>." }
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
                var videoFilePath = v_file_path; // 動画ファイルのパスを取得するためのコードをここに追加します

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

            //await FrameProcessor.Runpreview_apiAsync(outputPath, rectInfos, rectInfos.Length, BlackedOut_color_icon_color_info, FixedFrame_color_icon_color_Info, Inpaint.IsChecked.Value, Add_Copyright.IsChecked.Value, noInference: !No_Inference.IsChecked.Value);
            await FrameProcessor.Runpreview_apiAsync(outputPath, rectInfos, rectInfos.Length, BlackedOut_color_icon_color_info, FixedFrame_color_icon_color_Info, Add_Copyright.IsChecked.Value, (string)BlackedOut_ComboBox.SelectedValue, (string)FixedFrame_ComboBox.SelectedValue,(int)BlackedOutSlideBar.Value,(int)FixedFrameSlideBar.Value);

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

                string v_bitrate = ((int)BitrateSlideBar.Value).ToString()+"M";

                if ((start_time != 0) || (end_time != all_sec))
                {
                    FFMpeg_text.Text = "Trimming video process";

                    Trim_skip = false;
                    int duration = end_time - start_time;
                    //string arguments = $"-hwaccel \"{hwaccel}\" -ss \"{start_time}\" -t \"{duration}\" -i \"{v_file_path}\" -vcodec copy -acodec copy -f mp4 -b:v 11M -preset slow  \"{video_temp_filename_1}\" -y";
                    //string arguments = $"-hwaccel \"{hwaccel}\" -ss \"{start_time}\" -t \"{duration}\" -i \"{v_file_path}\" -vcodec copy -acodec copy -f mp4 -b:v \"{v_bitrate}\" -preset slow  \"{video_temp_filename_1}\" -y";
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
                FFMpeg_text.Text = "WoL Name detection process";

                StopButton.IsEnabled = true;
                stopwatch.Reset();
                stopwatch.Start();
                timer.Start();

                //if (Use_TensorRT.IsChecked == true && !No_Inference.IsChecked.Value)
                if (Use_TensorRT.IsChecked == true && (string)BlackedOut_ComboBox.SelectedValue != "No_Inference")
                {
                    //await FrameProcessor.RunTrtMainAsync(video_temp_filename_1, video_temp_filename_2, codec, hwaccel, v_width, v_height, v_fps, v_color_primaries, rectInfos, rectInfos.Length, BlackedOut_color_icon_color_info, FixedFrame_color_icon_color_Info, Inpaint.IsChecked.Value, Add_Copyright.IsChecked.Value, noInference: !No_Inference.IsChecked.Value);
                    await FrameProcessor.RunTrtMainAsync(video_temp_filename_1, video_temp_filename_2, codec, hwaccel, v_width, v_height, v_fps, v_color_primaries, rectInfos, rectInfos.Length, BlackedOut_color_icon_color_info, FixedFrame_color_icon_color_Info, Add_Copyright.IsChecked.Value, (string)BlackedOut_ComboBox.SelectedValue, (string)FixedFrame_ComboBox.SelectedValue, (int)BlackedOutSlideBar.Value, (int)FixedFrameSlideBar.Value, v_bitrate,preset);
                }
                else
                {
                    //await FrameProcessor.RunDmlMainAsync(video_temp_filename_1, video_temp_filename_2, codec, hwaccel, v_width, v_height, v_fps, v_color_primaries, rectInfos, rectInfos.Length, BlackedOut_color_icon_color_info, FixedFrame_color_icon_color_Info, Inpaint.IsChecked.Value, Add_Copyright.IsChecked.Value, noInference: !No_Inference.IsChecked.Value);
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
                        video_temp_filename_3= video_temp_filename_2;

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
                cancel_state= false;
                UIControl_enable_true();
                running_state = false;
            }
        }

        private async Task movie_audio_process(string audioFile1, string videoFile2, string outvideo, bool Trim_skip, string v_bitrate)
        {
            //ProgressBar.IsIndeterminate = true;
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
                //arguments = $" -i \"{audioFile1}\" -i \"{videoFile2}\" -c:v copy -c:a copy -movflags faststart \"{outvideo}\"";
                arguments = $" -i \"{audioFile1}\" -i \"{videoFile2}\" -c:v copy -c:a copy \"{outvideo}\"";

            }
            else
            {
                //arguments = $"-hwaccel \"{hwaccel}\" -i \"{audioFile1}\" -i \"{videoFile2}\" -vcodec \"{codec}\" -b:v 11M -preset slow \"{outvideo}\"";
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
            //ProgressBar.IsIndeterminate = false;
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

                //DispatcherQueue.TryEnqueue(() =>
                //{
                //    FFMpeg_text.Text = (data + Environment.NewLine);

                //});
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


                //savedRect =  new Rect(x, y, width, height); 
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
            InfoBar.Severity = InfoBarSeverity.Informational;
            InfoBar.Message = "Converting to TensorRT. Please wait about 360 seconds.";
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
            running_state= false;
            ProgressBar.IsIndeterminate = false;
            Console.WriteLine("変換成功");
            InfoBar.Severity = InfoBarSeverity.Success;
            InfoBar.Message = "TensorRT Engine build success!";
            InfoBar.Visibility = Visibility.Visible;
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
                    //Stroke = newColor,
                    //Fill = newColor,
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
        //private void FixedFrame_color_icon_ColorChanged(object sender, RoutedEventArgs e)
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
