#pragma once

// 動作／表情命名的存取。
//
// 命名檔就放在模型自己的資料夾裡（Hiyori.model3.json → Hiyori.annotations.json），
// 這樣把整個模型資料夾複製或分享出去時，命名會一起帶著走。
//
// zip 模型（Foo.zip）沒辦法照這條走 —— 壓縮檔是唯讀的容器。折衷是 sidecar：
// 命名檔寫在 zip **旁邊**（models/Foo.annotations.json）。讀取時 sidecar 優先，
// 沒有才讀 zip 內建的那一份，模型作者因此仍然可以把命名一起打包進去，
// 而使用者自己改的永遠落在 sidecar，zip 一個位元組都不會被動到。

#include <filesystem>
#include <string>
#include <vector>

#include "model_types.h"

namespace l2m {

class ModelAssets;

enum class AnnotationKind { Motions, Expressions };

// 由模型入口檔路徑推出命名檔路徑；裸命名入口（model.json → model.annotations.json）
// 與 zip（Foo.zip → Foo.annotations.json，落在 zip 旁邊）也適用
std::filesystem::path annotationsPathFor(const std::filesystem::path& modelEntryPath);

// 讀取命名檔。不存在、壞掉、欄位不合法都回傳空的，不讓一個壞檔擋住模型載入。
ModelAnnotations readAnnotations(const std::filesystem::path& modelEntryPath);

// 同上，但 sidecar 不存在時會退回容器內建的那一份（zip 模型才有差別；
// 資料夾模型的兩者本來就是同一個檔案）。
ModelAnnotations readAnnotations(const std::filesystem::path& modelEntryPath, const ModelAssets& assets);

// 寫入命名檔；全空時直接移除，不在模型資料夾裡留下沒有內容的檔案
void writeAnnotations(const std::filesystem::path& modelEntryPath, const ModelAnnotations& data);

// 套用單一項目的意義，回傳新的命名物件（空字串代表清除）
ModelAnnotations applyMeaning(const ModelAnnotations& current, AnnotationKind kind, const std::string& key, const std::string& meaning);

// 用預設名稱補齊命名（模型分頁的「使用預設名稱」按鈕）：
// 動作群組填群組名、群組內個別動作填檔名（去掉 .motion3.json 結尾）、
// 表情填表情名本身。**已經填過的命名一律保留** —— 這顆按鈕是「快速起步」，
// 整份覆蓋會把使用者親手打的意義靜靜砍掉。
// 填的鍵刻意對齊命名區的列：單一動作的群組不另列 #索引 那幾列，
// 這裡也就不替它們填 —— 否則命名檔裡會多出畫面上看不到、也改不掉的條目。
ModelAnnotations fillDefaultNames(const ModelInfo& model, const ModelAnnotations& current);

// 把舊版集中式的 userData/annotations.json 拆進各個模型資料夾。
//
// 舊檔以模型 id（models 目錄下的相對路徑）為 key，搬完後改名保留，
// 不直接刪掉，萬一對應錯了使用者還救得回來。回傳搬移成功的模型數。
int migrateLegacyAnnotations(const std::filesystem::path& legacyPath, const std::filesystem::path& modelsDir, const std::vector<std::string>& knownModelIds);

}  // namespace l2m
