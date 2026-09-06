# Live2D Cubism Core 取得說明

Cubism Core 是 Live2D 的**閉源**執行核心（`.moc3` 的載入與演算），
授權條款不允許把它放進本 repo（`third_party/CubismCore/` 已列入 `.gitignore`）。
建置前需要自行放置一次。

## 步驟

1. 到官方網站下載 **Cubism SDK for Native**（下載即代表同意 Live2D Proprietary Software License）：
   https://www.live2d.com/sdk/download/native/
   - 本專案目前對應的版本：**5-r.5**（與 `cmake/FetchCubismFramework.cmake` 釘的 Framework 版本一致）
2. 解壓後把以下內容複製到 `third_party/CubismCore/`：

   ```
   CubismSdkForNative-5-r.5/Core/include/Live2DCubismCore.h
     → third_party/CubismCore/include/Live2DCubismCore.h

   CubismSdkForNative-5-r.5/Core/lib/windows/x86_64/143/*.lib     （Windows / VS2022）
     → third_party/CubismCore/lib/windows/x86_64/143/

   CubismSdkForNative-5-r.5/Core/lib/macos/arm64/libLive2DCubismCore.a   （macOS）
     → third_party/CubismCore/lib/macos/arm64/
   CubismSdkForNative-5-r.5/Core/lib/macos/x86_64/libLive2DCubismCore.a
     → third_party/CubismCore/lib/macos/x86_64/

   CubismSdkForNative-5-r.5/Core/lib/linux/x86_64/libLive2DCubismCore.a  （Linux）
     → third_party/CubismCore/lib/linux/x86_64/

   CubismSdkForNative-5-r.5/Core/LICENSE.md
     → third_party/CubismCore/LICENSE.md
   ```

3. 重新執行 CMake configure。缺檔時 configure 會直接失敗並指向本文件。

## 注意事項

- Windows 的 `.lib` 依 MSVC runtime 分成 `_MD` / `_MDd` / `_MT` / `_MTd`，
  本專案用動態 runtime（`/MD`、Debug 為 `/MDd`），`cmake/SetupCubismCore.cmake`
  會依組態自動挑對。
- **發布**使用 Cubism Core 的應用程式需遵守 Live2D 的出版授權
  （小規模一般免費，詳見官方條款）：https://www.live2d.com/sdk/license/
- CubismNativeFramework（開源部分）由 CMake configure 時自動從 GitHub 取得，
  不需手動處理。
