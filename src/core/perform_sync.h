#pragma once

// 「跟著聲音一起演」：perform 的哪些步驟該押後到 TTS 真的出聲那一刻。
//
// 症狀。AI 的典型序列是 [expression, motion, speak]，而 PerformRunner 是嚴格的
// 一步一步狀態機 —— motion 同步回傳、在 t=0 就播掉了，speak 這時才把合成請求
// 送出去，兩件事零重疊。實測（三月七 + nod，custom 引擎，直接對 MCP 打 curl）：
// [motion] 本身只要 7 ms，[motion, speak(wait:false)] 要 2.35 / 2.48 秒 ——
// 也就是點完頭之後有兩秒半既沒有聲音也沒有氣泡（氣泡本來就等第一個 PCM，
// 見 app/speech_controller.h 的四個入口），然後聲音才自己冒出來。
// 動作早就演完了，看起來像對不上嘴的爛配音。
//
// 修法照抄氣泡那條已經對了的路：**把純視覺的步驟押後到出聲那一刻**，
// 跟氣泡同一拍放出來。這裡只負責「哪些步驟可以押後」這個純判斷，
// 押後與放行的時序在 app/perform_runner.h。
//
// 兩條規則：
//   ① 只押後「角色長什麼樣」的步驟（motion／expression／parameters／animate）。
//      move 不算 —— 那是位置變更，不是配合台詞的表演，而且它可能是非同步的
//      滑行；wait 更不算，它本身就是 AI 明寫的節奏（「動一下、頓半秒、再開口」），
//      押後等於把那個 wait 抹掉。
//   ② 中間只准隔著同類步驟。遇到 move／wait／未知 action 就整組不押後 ——
//      那些步驟會照原順序自己執行，押後的視覺步驟跳過它們就變成真的亂序。
//
// bubbleOnly 的 speak（自主表演的嘀咕）刻意**不算**命中：它走 mutter，
// 氣泡當場就出來、根本沒有那段空窗，押後只會把單純的事情弄複雜。
//
// 純函式放這裡而不是 perform_runner.cpp 的 static：這是「哪一步在哪一刻演」
// 的規則本身，判錯的兩個方向都是靜默的 —— 少押後就是問題原封不動，
// 多押後（例如把 wait 也吃進去）就是 AI 寫的節奏被悄悄抹掉、動作全擠在一拍。

#include <cstddef>
#include <string>
#include <vector>

#include "perform_step.h"

namespace l2m {

// 這個 action 是不是「純視覺」的伴奏步驟（規則①）
bool isSpeechCompanionAction(const std::string& action);

// steps[index] 該不該押後到下一個 speak 步驟真的出聲時才執行
bool deferUntilSpeech(const std::vector<PerformStep>& steps, size_t index);

}  // namespace l2m
