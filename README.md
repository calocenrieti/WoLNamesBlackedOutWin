# WoLNamesBlackedOutWin
FF14の動画や画像からキャラクター名を隠すWindowsアプリです。<br>
動画処理にはFFmpegを利用しています。<br>
ONNX推論にはWindowsMLを利用しています<br>
<br>
アプリのインストールはMicrosoftストアから<br>
https://apps.microsoft.com/detail/9P5W7QTSH297?hl=ja-jp&gl=JP&ocid=pdpshare
<br>
アプリの操作説明はこちらから<br>
https://blog.calocenrieti.com/blog/wol_names_blacked_out_win/

## 技術的ポイント
- Ultralytics yolo26をデータセットを準備してPythonで学習、ONNXにエクスポート、onnxsimでシンプル化、Modeloptで入出力以外をFP16に変更。
- UIをC# WinUI3、ONNX推論や動画入出力をC++で作成
- ONNX推論にはWindowsMLを利用。TensorRT-RTX、MigraphX、OpenVINO、DirectMLの中から使えるものを使う。
- 動画入出力にはFFmpeg dllを利用。
- 動画入力から前処理、推論、後処理、動画出力をGPU内で完結するzero-copyでパフォーマンス改善しています。（但しIntel QSV利用時は部分的にCPUコピーが発生する）
- 動画入出力、推論、OCRをスレッド対応しています。
- PaddleOCRを利用し名前をOCRしてマスク対象外を判定しています。
- BYTETRACKで追跡することでOCR頻度を下げてパフォーマンス対策しています。
- AIが9割、人間が1割くらいでコード書きました。

## License
This project is licensed under the LGPL v2.1 (or later).

The distribution includes third-party libraries under their respective licenses.
See the LICENSES folder for details.

## Third Party Libraries & Licenses

This project incorporates the following third-party components:

- **[FFmpeg](https://ffmpeg.org/)** (LGPLv2.1)
- **[ByteTrack-cpp](https://github.com/derpda/ByteTrack-cpp)** (MIT)
- **[Eigen](https://gitlab.com/libeigen/eigen)** (MPL 2.0)

- **[Microsoft.Windows.CppWinRT](https://www.nuget.org/packages/Microsoft.Windows.CppWinRT)** ([License](https://www.nuget.org/packages/Microsoft.Windows.CppWinRT/3.0.260715.1/License))
- **[Microsoft.Windows.SDK.BuildTools](https://www.nuget.org/packages/Microsoft.Windows.SDK.BuildTools)** ([License](https://aka.ms/WinSDKLicenseURL))
- **[Microsoft.WindowsAppSDK](https://www.nuget.org/packages/Microsoft.WindowsAppSDK)** ([License](https://www.nuget.org/packages/Microsoft.WindowsAppSDK/2.3.1/License))
- **[Microsoft.Windows.AI.MachineLearning](https://www.nuget.org/packages/Microsoft.Windows.AI.MachineLearning)** ([License](https://www.nuget.org/packages/Microsoft.Windows.AI.MachineLearning/2.2.12/License))
- **[Microsoft.WindowsAppSDK.AI](https://www.nuget.org/packages/Microsoft.WindowsAppSDK.AI)** ([License](https://www.nuget.org/packages/Microsoft.WindowsAppSDK.AI/2.3.4/License))
- **[Microsoft.WindowsAppSDK.Base](https://www.nuget.org/packages/Microsoft.WindowsAppSDK.Base)** ([License](https://www.nuget.org/packages/Microsoft.WindowsAppSDK.Base/2.0.4/License))
- **[Microsoft.WindowsAppSDK.DWrite](https://www.nuget.org/packages/Microsoft.WindowsAppSDK.DWrite)** ([License](https://www.nuget.org/packages/Microsoft.WindowsAppSDK.DWrite/2.1.0/License))
- **[Microsoft.WindowsAppSDK.Foundation](https://www.nuget.org/packages/Microsoft.WindowsAppSDK.Foundation)** ([License](https://www.nuget.org/packages/Microsoft.WindowsAppSDK.Foundation/2.3.5/License))
- **[Microsoft.WindowsAppSDK.InteractiveExperien](https://www.nuget.org/packages/Microsoft.WindowsAppSDK.InteractiveExperiences)** ([License](https://www.nuget.org/packages/Microsoft.WindowsAppSDK.InteractiveExperiences/2.1.3/License))
- **[Microsoft.WindowsAppSDK.ML](https://www.nuget.org/packages/Microsoft.WindowsAppSDK.ML)** ([License](https://www.nuget.org/packages/Microsoft.WindowsAppSDK.ML/2.1.74/License))
- **[Microsoft.WindowsAppSDK.Runtime](https://www.nuget.org/packages/Microsoft.WindowsAppSDK.Runtime)** ([License](https://www.nuget.org/packages/Microsoft.WindowsAppSDK.Runtime/2.3.1/License))
- **[Microsoft.WindowsAppSDK.Widgets](https://www.nuget.org/packages/Microsoft.WindowsAppSDK.Widgets)** ([License](https://www.nuget.org/packages/Microsoft.WindowsAppSDK.Widgets/2.0.5/License))
- **[Microsoft.WindowsAppSDK.WinUI](https://www.nuget.org/packages/Microsoft.WindowsAppSDK.WinUI)** ([License](https://www.nuget.org/packages/Microsoft.WindowsAppSDK.WinUI/2.3.2/License))
- **[Microsoft.Web.WebView2](https://www.nuget.org/packages/Microsoft.Web.WebView2)** ([License](https://www.nuget.org/packages/Microsoft.Web.WebView2/1.0.4129.50/License))
- **[System.Numerics.Tensors](https://www.nuget.org/packages/System.Numerics.Tensors/)** (MIT)

- **[Ultralytics YOLO26](https://github.com/ultralytics/ultralytics)** (AGPL-3.0)<br>
  *Note: Only the exported ONNX model is used. This project itself is not licensed under AGPL.*
- **[PaddleOCR](https://github.com/PaddlePaddle/PaddleOCR)** (Apache 2.0)<br>
  This project utilizes the en_PP-OCRv5_mobile_rec recognition model exported to ONNX.
