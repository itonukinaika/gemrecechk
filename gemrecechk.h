/*
 * Copyright (C) 2026 Hajime Segawa / 糸貫内科クリニック
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 *
 * ---
 * Acknowledge: This code was developed with the assistance of Gemini 3 Pro (Google AI).
 */

#ifndef GEMRECECHK_H
#define GEMRECECHK_H

#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif

#include <windows.h>
#include <commctrl.h>
#include <winhttp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "resource.h"
#include "cJSON.h"
#include "ezxml.h"

// 定数定義
#define MAX_PATH_LEN 512
#define MAX_SETTING_VAL 1024
#define INSURANCE_SHAHO 1
#define INSURANCE_KOKUHO 2
#define INSURANCE_KOUKI 3
#define GEMINI_HOST L"generativelanguage.googleapis.com"

// 明細情報構造体 (1つの薬剤や手技)
typedef struct Medication_Info {
    char Medication_Name[256];
    char Medication_Name_Input_Value[256];
    char Medication_Number[32];
    char Unit_Code_Name[32];
    char Medication_Point[32];
    char Medication_Code[32];
    struct Medication_Info* next;
} Medication_Info;

// 剤情報構造体 (【内服】などのグループ)
typedef struct Medical_Class_Info {
    char Medical_Class[16];
    char Medical_Class_Name[64];
    char Medical_Class_Number[32];
    char Medical_Class_Point[32];
    int is_medication;
    Medication_Info* details_head;
    struct Medical_Class_Info* next;
} Medical_Class_Info;

// レセプトデータ構造体
typedef struct Patient_Data {
    char Patient_ID[32];
    char WholeName[128];
    char BirthDate[32];
    int Age;
    int Insurance_Flag; // 1:社保, 2:国保, 3:後期
    char* Disease_List;      // 動的確保 (成型済みテキスト)
    char* Medical_Info_List; // 動的確保 (成型済みテキスト)
    char* Gemini_Response;   // 動的確保 (Geminiからの返答テキスト)
} Patient_Data;

// アプリケーション設定構造体
typedef struct App_Settings {
    char ORCAAddr[MAX_SETTING_VAL];
    char ORCAUser[MAX_SETTING_VAL];
    char ORCAPassword[MAX_SETTING_VAL];
    char GeminiKey[MAX_SETTING_VAL];
    char GeminiModel[MAX_SETTING_VAL];
    char GEMSysPrompt[MAX_SETTING_VAL * 2];
    int GEMSplitCase;
    int Debug;
    int Priority;
    char GEMUserPrompt[MAX_SETTING_VAL * 32];
} App_Settings;

// --- グローバル変数 ---
extern App_Settings g_settings;
extern Patient_Data* g_patients;
extern int g_patient_count;
extern HWND g_hMainWnd;
extern HANDLE g_hStopEvent;

// --- ヘルパー関数のプロトタイプ ---
LPWSTR Utf8ToUtf16(const char* utf8);
char* Utf16ToUtf8(LPCWSTR utf16);
void DebugPrintf(const char* format, ...);

// --- カスタムメッセージの定義 ---
#define WM_USER_PROGRESS (WM_APP + 1)
#define WM_USER_FINISHED (WM_APP + 2)

// --- スレッド引数の構造体 ---
typedef struct {
    int target_year;
    int target_month;
    int target_day;
    HWND hProgressDlg;
} OrcaThreadArgs;

typedef struct {
    int* target_indices;
    int target_count;
    HWND hProgressDlg;
} GeminiThreadArgs;

// --- 各モジュールの関数プロトタイプ ---
void AddPatientDeduplicated(const char* pid, const char* name, const char* dob, int target_year, int target_month);
unsigned __stdcall FetchOrcaDataThread(void* param);
unsigned __stdcall RunGeminiCheckThread(void* param);

#endif