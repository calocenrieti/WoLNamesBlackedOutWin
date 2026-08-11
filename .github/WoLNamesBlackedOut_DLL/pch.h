// pch.h: プリコンパイル済みヘッダー ファイルです。
// 次のファイルは、その後のビルドのビルド パフォーマンスを向上させるため 1 回だけコンパイルされます。
// コード補完や多くのコード参照機能などの IntelliSense パフォーマンスにも影響します。
// ただし、ここに一覧表示されているファイルは、ビルド間でいずれかが更新されると、すべてが再コンパイルされます。
// 頻繁に更新するファイルをここに追加しないでください。追加すると、パフォーマンス上の利点がなくなります。

#ifndef PCH_H
#define PCH_H

// プリコンパイルするヘッダーをここに追加します
#include "framework.h"
#include "DebugLog.h"

// WinML 2.0 関連
#include <winrt/Windows.Graphics.DirectX.Direct3D11.h>
#include <winrt/Microsoft.Windows.AI.MachineLearning.h>
#include <winrt/Windows.Media.h>
#include <winrt/Windows.Graphics.DirectX.h>
#include <winrt/Windows.Storage.h>

// Windows Runtime 初期化
#include <roapi.h>

// WIC for image loading
#include <wincodec.h>

#endif //PCH_H
