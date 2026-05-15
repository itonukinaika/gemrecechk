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

// ウィンドウプロシージャ群と、エントリーポイント

#include "gemrecechk.h"
#include <commctrl.h>
#include <shlwapi.h>
#include <shellapi.h>
#include <process.h>
#include <commdlg.h>

// 印刷関数のプロトタイプ宣言
void PrintSummaryList(HWND hwnd, int* filtered_indices, int filtered_count);

// ダイアログプロシージャのプロトタイプ宣言
INT_PTR CALLBACK ConfigDlgProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);
INT_PTR CALLBACK ProgressDlgProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);
INT_PTR CALLBACK ViewerDlgProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);
INT_PTR CALLBACK MainDlgProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);

// グローバル変数
App_Settings g_settings = {0};
Patient_Data* g_patients = NULL;
int g_patient_count = 0;
HWND g_hMainWnd = NULL;
HANDLE g_hStopEvent = NULL;
// ビューア用変数
int g_current_view_idx = 0;
int* g_filtered_indices = NULL;
int g_filtered_count = 0;
// ライセンス表示用
typedef struct {
    const char* title;
    const char* filename;
} LicenseInfo;

// ============================================================================
// ヘルパー関数群
// ============================================================================

// UTF-8文字列をUTF-16(ワイド文字列)に変換 (戻り値は free() で解放すること)
LPWSTR Utf8ToUtf16(const char* utf8) {
    if (!utf8) return NULL;
    int len = MultiByteToWideChar(CP_UTF8, 0, utf8, -1, NULL, 0);
    LPWSTR utf16 = (LPWSTR)calloc(len, sizeof(WCHAR));
    MultiByteToWideChar(CP_UTF8, 0, utf8, -1, utf16, len);
    return utf16;
}

// UTF-16(ワイド文字列)をUTF-8文字列に変換 (戻り値は free() で解放すること)
char* Utf16ToUtf8(LPCWSTR utf16) {
    if (!utf16) return NULL;
    int len = WideCharToMultiByte(CP_UTF8, 0, utf16, -1, NULL, 0, NULL, NULL);
    char* utf8 = (char*)calloc(len, sizeof(char));
    WideCharToMultiByte(CP_UTF8, 0, utf16, -1, utf8, len, NULL, NULL);
    return utf8;
}

// Shift-JIS文字列をUTF-8に変換 (戻り値は free() で解放すること)
char* SjisToUtf8(const char* sjis) {
    int lenW = MultiByteToWideChar(932, 0, sjis, -1, NULL, 0);
    LPWSTR wstr = (LPWSTR)malloc(lenW * sizeof(WCHAR));
    MultiByteToWideChar(932, 0, sjis, -1, wstr, lenW);
    int len8 = WideCharToMultiByte(CP_UTF8, 0, wstr, -1, NULL, 0, NULL, NULL);
    char* utf8 = (char*)malloc(len8);
    WideCharToMultiByte(CP_UTF8, 0, wstr, -1, utf8, len8, NULL, NULL);
    free(wstr);
    return utf8;
}

// 西暦(YYYYMMDD)から西暦(YYYY-MM-DD)に変換
void ConvertBirthDate(const char* raw_date, char* out_date) {
    // 8桁(YYYYMMDD)でない場合はデフォルト値をセット
    if (strlen(raw_date) != 8) {
        strcpy(out_date, "1900-01-01");
        return;
    }
    sprintf(out_date, "%.4s-%.2s-%.2s", raw_date, raw_date + 4, raw_date + 6);
}

// ============================================================================
// デバッグログ出力関数 (debug.txt を exe と同一ディレクトリに出力)
// ============================================================================
void DebugPrintf(const char* format, ...) {
    if (!g_settings.Debug) return;

    // カレントディレクトリが変更されても、debug.txtが実行ファイルと同一ディレクトリに作成されるようにする
    static WCHAR wDebugPath[MAX_PATH] = L"";
    if (wDebugPath[0] == L'\0') {
        // 実行ファイルのフルパスを取得
        GetModuleFileNameW(NULL, wDebugPath, MAX_PATH);
        // ファイル名の部分を削除してディレクトリ名のみにする
        WCHAR* lastBackslash = wcsrchr(wDebugPath, L'\\');
        if (lastBackslash) {
            *(lastBackslash + 1) = L'\0';
            wcscat(wDebugPath, L"debug.txt");
        }
    }

    static int is_first_call = 1;
    const WCHAR* wMode = is_first_call ? L"w" : L"a";
    is_first_call = 0;

    // _wfopen を使用してフルパスでファイルを開く
    FILE* fp = _wfopen(wDebugPath, wMode);
    if (fp != NULL) {
        va_list args;
        va_start(args, format);
        vfprintf(fp, format, args);
        va_end(args);
        fclose(fp);
    }
}

// ============================================================================
// gemprompt.txt の読み込みと、無ければ記載例を作成
// ============================================================================
void LoadUserPrompt() {
    FILE* fp = fopen("gemprompt.txt", "r");
    if (!fp) {
        fp = fopen("gemprompt.txt", "w");
        if (fp) {
            fprintf(fp, 
                "# ユーザー設定プロンプト　施設にあわせての調整は必須です\n"
                "# #で始まる行はコメントで、Geminiには送信されません\n"
                "\n"
                "# 全体としての判定基準\n"
                "処方、検査、処置と病名の対応を、厳密に確認してください。不足している病名が予想できれば、提案してください。\n"
                "小児科外来診療料が算定されている場合は、包括で算定されるため、病名の開始日に矛盾がなければ、診療内容に関わらずOKです。\n"
                "病名に転帰がなされていても、同月内の診療行為に対しては有効と判断してください。\n"
                "同月内に複数回受診がある場合は、再診料などが重複します。同月内に複数回算定可能な診察料は無視してください。\n"
                "\n"
                "# 病名判定基準\n"
                "補液が行われた場合、薬剤を溶解して投与するために生食100mLが必要であった場合を除き、脱水症等の病名が必要です。\n"
                "麻薬等加算は、向精神薬や覚醒剤原料(セルトラリン、エフェドリン含有製剤の一部等)でも算定できます。\n"
                "整腸剤(ビオフェルミンなど)は便通異常(便秘、下痢)でも算定できます。また、抗菌薬とセットで用いる場合は病名不要です。\n"
                "抗菌外用薬(ステロイドと抗菌薬の合剤を除く)には、何らかの皮膚細菌感染症か、皮膚疾患や外傷と細菌二次感染を組み合わせた病名が必要です。\n"
                "胃潰瘍の病名があり、NSAIDs(内服、貼付薬)のうち、胃潰瘍には禁忌となる処方がある場合は警告してください。\n"
                "\n"
                "# 病名以外の警告基準\n"
                "末尾に(主)がついているものが、主病名です。生活習慣病管理料が算定されている場合は、正しい主病名がついている病名があるか確認してください。\n"
                "初診を算定している場合、その前月以前からの病名があれば、警告してください。\n"
                "有効な病名が全くないのに、再診料を算定している場合は、警告してください。\n"
                "急性期病名や疑い病名が長期間継続している場合は、警告してください。\n"
                "10件以上の未転帰病名がある場合は、同月の診療行為に対して必要なものと、生活習慣病を除いて、不要な可能性のある病名があれば、提案してください。\n"
                "生活習慣病の病名が3ヶ月以上継続していて、これに対応する処方があり、主病名がなく、生活習慣病管理料が算定されていない場合は、算定を提案してください。\n"
            );
            fclose(fp);
        }
        fp = fopen("gemprompt.txt", "r");
        if (!fp) {
            g_settings.GEMUserPrompt[0] = '\0';
            return;
        }
    }
    
    // ファイル読み込み処理
    g_settings.GEMUserPrompt[0] = '\0';
    char line[2048];
    size_t current_len = 0;
    size_t max_len = sizeof(g_settings.GEMUserPrompt) - 1;

    while (fgets(line, sizeof(line), fp)) {
        // 行の先頭が '#' の場合はコメントとして無視
        if (line[0] == '#') {
            continue;
        }
        
        size_t line_len = strlen(line);
        if (current_len + line_len < max_len) {
            strcat(g_settings.GEMUserPrompt, line);
            current_len += line_len;
        } else {
            // バッファサイズ(ひとまず32kb)に収まる分だけ連結
            strncat(g_settings.GEMUserPrompt, line, max_len - current_len);
            break;
        }
    }
    fclose(fp);
}

// ============================================================================
// ライセンス同意ダイアログ プロシージャ
// ============================================================================
INT_PTR CALLBACK LicenseDlgProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
        case WM_INITDIALOG: {
            LicenseInfo* info = (LicenseInfo*)lParam;
            
            LPWSTR wTitle = Utf8ToUtf16(info->title);
            SetDlgItemTextW(hwnd, IDC_LBL_LICENSE_TITLE, wTitle);
            free(wTitle);
            
            // ライセンスファイルを読み込んで表示。見つからない場合は終了。
            FILE* fp = fopen(info->filename, "rb");
            if (!fp) {
                LPWSTR wFilename = Utf8ToUtf16(info->filename);
                WCHAR errorMsg[512];
                wsprintfW(errorMsg, L"ライセンスファイルが読み込めません: %s\nアプリケーションを終了します。", wFilename);
                MessageBoxW(hwnd, errorMsg, L"エラー", MB_OK | MB_ICONERROR);
                free(wFilename);
                EndDialog(hwnd, -1);
                return TRUE;
            }
            
            fseek(fp, 0, SEEK_END);
            long size = ftell(fp);
            fseek(fp, 0, SEEK_SET);
            
            char* buffer = (char*)malloc(size + 1);
            fread(buffer, 1, size, fp);
            buffer[size] = '\0';
            fclose(fp);
            
            // LFをCRLFに変換
            long crlf_count = 0;
            for(long i = 0; i < size; i++) {
                if (buffer[i] == '\n' && (i == 0 || buffer[i-1] != '\r')) crlf_count++;
            }
            char* crlf_buffer = (char*)malloc(size + crlf_count + 1);
            long j = 0;
            for(long i = 0; i < size; i++) {
                if (buffer[i] == '\n' && (i == 0 || buffer[i-1] != '\r')) {
                    crlf_buffer[j++] = '\r';
                }
                crlf_buffer[j++] = buffer[i];
            }
            crlf_buffer[j] = '\0';
            
            LPWSTR wText = Utf8ToUtf16(crlf_buffer);
            SetDlgItemTextW(hwnd, IDC_TXT_LICENSE_TEXT, wText);
            free(wText);
            free(crlf_buffer);
            free(buffer);
            
            // テキストが全選択されるのを防ぎ、カーソルを先頭にする
            SendDlgItemMessageW(hwnd, IDC_TXT_LICENSE_TEXT, EM_SETSEL, 0, 0);
            // デフォルトではキャンセルになる。ユーザーが明示的に同意をクリックするか、フォーカスを合わせないと同意できない
            SetFocus(GetDlgItem(hwnd, IDC_BTN_DECLINE));
            
            return FALSE; // システムによるデフォルトのフォーカス設定を無効化
        }
        case WM_COMMAND:
            if (LOWORD(wParam) == IDC_BTN_AGREE) {
                EndDialog(hwnd, 1);
                return TRUE;
            }
            if (LOWORD(wParam) == IDC_BTN_DECLINE || LOWORD(wParam) == IDCANCEL) {
                EndDialog(hwnd, 0);
                return TRUE;
            }
            break;
    }
    return FALSE;
}

// ============================================================================
// gemprompt.ini の読み込みと、無ければライセンス同意後に作成
// ============================================================================
int LoadSettings() {
    FILE* fp = fopen("gemrecechk.ini", "r");
    if (!fp) {
        // 初回起動(iniファイルが無いとき)は、ライセンス同意画面の表示
        LicenseInfo licenses[] = {
            {"gemrecechkは、いかなる場合も＊完全に無保証＊です。以下のライセンスを確認してください。", "gemrecechk-LICENSE.txt"},
            {"このプロジェクトはezxmlライブラリを使用しています。以下のライセンスを確認してください。", "ezxml-LICENSE.txt"},
            {"このプロジェクトはcJSONライブラリを使用しています。以下のライセンスを確認してください。", "cJSON-LICENSE.txt"}
        };
        
        for (int i = 0; i < 3; i++) {
            INT_PTR res = DialogBoxParamW(GetModuleHandle(NULL), MAKEINTRESOURCEW(IDD_LICENSE), NULL, LicenseDlgProc, (LPARAM)&licenses[i]);
            if (res <= 0) {
                // キャンセルされた、またはファイルが見つからなかった場合
                exit(0);
            }
        }

    	// ライセンスに同意したらiniファイル作成
        fp = fopen("gemrecechk.ini", "w");
        if (fp) {
            fprintf(fp, "orcaaddr=http://192.168.0.0:8000/api/api01rv2/\n");
            fprintf(fp, "orcauser=\n");
            fprintf(fp, "orcapassword=\n");
            fprintf(fp, "geminikey=\n");
            fprintf(fp, "geminimodel=gemini-pro-latest\n");
            fprintf(fp, "gemsysprompt=以下は保険診療のレセプトデータです。提供されるtemp_id(仮ID)、disease_list(病名と開始日の一覧)、medical_info(診療行為の一覧)を比較し、処方薬や実施した検査・処置に対する病名の不足、または不備を指摘し、該当するtemp_idと不備の具体的な内容(deficiency)を報告してください。不足や不備がないtemp_idについては、deficiencyにOKとだけ返答してください。\n");
            fprintf(fp, "gemsplitcase=10\n");
            fprintf(fp, "debug=no\n");
            fprintf(fp, "priority=no\n");
            fclose(fp);
        }
    } else {
        fclose(fp);
    }
    
	// 何かの間違いでiniファイルからモデル名やシステムプロンプト名を削除した時のために、デフォルト値をセット
    strcpy(g_settings.GeminiModel, "gemini-pro-latest");
    strcpy(g_settings.GEMSysPrompt, "以下は保険診療のレセプトデータです。提供されるtemp_id(仮ID)、disease_list(病名と開始日の一覧)、medical_info(診療行為の一覧)を比較し、処方薬や実施した検査・処置に対する病名の不足、または不備を指摘し、該当するtemp_idと不備の具体的な内容(deficiency)を報告してください。不足や不備がないtemp_idについては、deficiencyにOKとだけ返答してください。");
    g_settings.GEMSplitCase = 10;
    g_settings.Debug = 0;
    g_settings.Priority = 0;

    fp = fopen("gemrecechk.ini", "r");
    if (fp) {
        char line[2048];
        while (fgets(line, sizeof(line), fp)) {
            char* p = strchr(line, '\n'); if (p) *p = '\0';
            p = strchr(line, '\r'); if (p) *p = '\0';
            
            char* eq = strchr(line, '=');
            if (!eq) continue;
            *eq = '\0';
            char* key = line;
            char* val = eq + 1;
            
            if (strcmp(key, "orcaaddr") == 0) strcpy(g_settings.ORCAAddr, val);
            else if (strcmp(key, "orcauser") == 0) strcpy(g_settings.ORCAUser, val);
            else if (strcmp(key, "orcapassword") == 0) strcpy(g_settings.ORCAPassword, val);
            else if (strcmp(key, "geminikey") == 0) strcpy(g_settings.GeminiKey, val);
            else if (strcmp(key, "geminimodel") == 0) strcpy(g_settings.GeminiModel, val);
            else if (strcmp(key, "gemsysprompt") == 0) strcpy(g_settings.GEMSysPrompt, val);
            else if (strcmp(key, "gemsplitcase") == 0) g_settings.GEMSplitCase = atoi(val);
            else if (strcmp(key, "debug") == 0) g_settings.Debug = (strcmp(val, "yes") == 0) ? 1 : 0;
            else if (strcmp(key, "priority") == 0) g_settings.Priority = (strcmp(val, "yes") == 0) ? 1 : 0;
        }
        fclose(fp);
    }
    
    LoadUserPrompt();
    
    int missing = 0;
    char msg[1024] = {0};
    if (strlen(g_settings.ORCAUser) == 0) { strcat(msg, "orcauserが設定されていません。\n"); missing = 1; }
    if (strlen(g_settings.ORCAPassword) == 0) { strcat(msg, "orcapasswordが設定されていません。\n"); missing = 1; }
    if (strlen(g_settings.GeminiKey) == 0) { strcat(msg, "geminikeyが設定されていません。\n"); missing = 1; }
    
    if (missing) {
        strcat(msg, "設定画面で設定してください。");
        LPWSTR wmsg = Utf8ToUtf16(msg);
        MessageBoxW(NULL, wmsg, L"設定エラー", MB_ICONWARNING | MB_OK);
        free(wmsg);
        return 0; 
    }
    return 1;
}

// ============================================================================
// UKEファイルから患者一覧を読み込む関数群
// ============================================================================

// RE行のパースと登録
void ParseReLineAndAddPatient(char* line, int target_year, int target_month) {
    char* tokens[30] = {0};
    int count = 0;
    char* p = line;
    tokens[count++] = p;
    while (*p && count < 30) {
        if (*p == ',') {
            *p = '\0';
            tokens[count++] = p + 1;
        }
        p++;
    }
    // RE行で、十分なカラムがあるか確認
    if (count > 13 && strcmp(tokens[0], "RE") == 0) {
        char* sjis_name = tokens[4];
        char* birth_raw = tokens[6];
        char* pat_id = tokens[13];
        
        char* utf8_name = SjisToUtf8(sjis_name);
        char birth_date[32];
        ConvertBirthDate(birth_raw, birth_date);
        
        AddPatientDeduplicated(pat_id, utf8_name, birth_date, target_year, target_month);
        free(utf8_name);
    }
}

// UKEファイルの選択と読み込み
int SelectAndLoadUkeFile(HWND hwnd, const WCHAR* prompt, int target_year, int target_month) {
    if (MessageBoxW(hwnd, prompt, L"確認", MB_OKCANCEL | MB_ICONINFORMATION) != IDOK) {
        return 0;
    }
    
    WCHAR szFile[MAX_PATH] = {0};
    OPENFILENAMEW ofn = {0};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = hwnd;
    ofn.lpstrFilter = L"レセ電ファイル (*.UKE)\0*.UKE\0すべてのファイル (*.*)\0*.*\0";
    ofn.lpstrFile = szFile;
    ofn.nMaxFile = MAX_PATH;
    ofn.Flags = OFN_PATHMUSTEXIST | OFN_FILEMUSTEXIST;
    
    if (GetOpenFileNameW(&ofn)) {
        FILE* fp = _wfopen(szFile, L"r");
        if (!fp) {
            MessageBoxW(hwnd, L"ファイルを開けませんでした。", L"エラー", MB_OK | MB_ICONERROR);
            return 0;
        }
        char line[4096];
        while (fgets(line, sizeof(line), fp)) {
            char* p = strchr(line, '\r'); if(p) *p = '\0';
            p = strchr(line, '\n'); if(p) *p = '\0';
            if (strncmp(line, "RE,", 3) == 0) {
                ParseReLineAndAddPatient(line, target_year, target_month);
            }
        }
        fclose(fp);
        return 1;
    }
    return 0; // キャンセルされた
}

// 既存データ破棄
void ClearPatientData() {
    if (g_patients) {
        for (int i = 0; i < g_patient_count; i++) {
            if (g_patients[i].Disease_List) free(g_patients[i].Disease_List);
            if (g_patients[i].Medical_Info_List) free(g_patients[i].Medical_Info_List);
            if (g_patients[i].Gemini_Response) free(g_patients[i].Gemini_Response);
        }
        free(g_patients);
        g_patients = NULL;
        g_patient_count = 0;
    }
}

// スレッド実行の共通ヘルパー
void StartFetchThread(HWND hwnd, int year, int month, int day) {
    ResetEvent(g_hStopEvent);
    OrcaThreadArgs* args = (OrcaThreadArgs*)malloc(sizeof(OrcaThreadArgs));
    args->target_year = year;
    args->target_month = month;
    args->target_day = day;
    args->hProgressDlg = NULL;

    ShowWindow(hwnd, SW_HIDE);
    HANDLE hThread = (HANDLE)_beginthreadex(NULL, 0, FetchOrcaDataThread, args, 0, NULL);
    if (hThread) CloseHandle(hThread);

    INT_PTR res = DialogBoxParamW(GetModuleHandle(NULL), MAKEINTRESOURCEW(IDD_PROGRESS), hwnd, ProgressDlgProc, (LPARAM)&args->hProgressDlg);

    if (res == 1) {
        MessageBoxW(hwnd, L"レセプトデータの取得を中止しました。", L"中止", MB_ICONINFORMATION | MB_OK);
    } else if (g_patient_count == 0) {
        MessageBoxW(hwnd, L"該当する期間のレセプトデータが見つかりませんでした。", L"情報", MB_ICONINFORMATION | MB_OK);
    } else {
        DialogBoxParamW(GetModuleHandle(NULL), MAKEINTRESOURCEW(IDD_VIEWER), hwnd, ViewerDlgProc, 0);
    }

    ShowWindow(hwnd, SW_SHOW);
}

// ============================================================================
// メインウィンドウのプロシージャ
// ============================================================================
INT_PTR CALLBACK MainDlgProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
        case WM_INITDIALOG: {
            g_hMainWnd = hwnd;
            
            SYSTEMTIME st;
            GetLocalTime(&st);
            
            int target_year = st.wYear;
            int target_month = st.wMonth;
            if (target_month == 0) {
                target_month = 12;
                target_year -= 1;
            }

            WCHAR buf[32];
            HWND hCbYear = GetDlgItem(hwnd, IDC_YEAR);
            for (int y = st.wYear - 10; y <= st.wYear + 1; y++) {
                wsprintfW(buf, L"%d", y);
                int idx = SendMessageW(hCbYear, CB_ADDSTRING, 0, (LPARAM)buf);
                if (y == target_year) SendMessageW(hCbYear, CB_SETCURSEL, idx, 0);
            }
            
            HWND hCbMonth = GetDlgItem(hwnd, IDC_MONTH);
            for (int m = 1; m <= 12; m++) {
                wsprintfW(buf, L"%02d", m);
                int idx = SendMessageW(hCbMonth, CB_ADDSTRING, 0, (LPARAM)buf);
                if (m == target_month) SendMessageW(hCbMonth, CB_SETCURSEL, idx, 0);
            }
            
            HWND hCbDay = GetDlgItem(hwnd, IDC_DAY);
            SendMessageW(hCbDay, CB_ADDSTRING, 0, (LPARAM)L"全て");
            SendMessageW(hCbDay, CB_SETCURSEL, 0, 0); 
            for (int d = 1; d <= 31; d++) {
                wsprintfW(buf, L"%02d", d);
                SendMessageW(hCbDay, CB_ADDSTRING, 0, (LPARAM)buf);
            }
            return TRUE;
        }
        case WM_COMMAND: {
            int wmId = LOWORD(wParam);
            if (wmId == IDC_BTN_TODAY) {
                SYSTEMTIME st;
                GetLocalTime(&st);
                WCHAR buf[32];
                wsprintfW(buf, L"%d", st.wYear);
                SendMessageW(GetDlgItem(hwnd, IDC_YEAR), CB_SELECTSTRING, -1, (LPARAM)buf);
                wsprintfW(buf, L"%02d", st.wMonth);
                SendMessageW(GetDlgItem(hwnd, IDC_MONTH), CB_SELECTSTRING, -1, (LPARAM)buf);
                wsprintfW(buf, L"%02d", st.wDay);
                SendMessageW(GetDlgItem(hwnd, IDC_DAY), CB_SELECTSTRING, -1, (LPARAM)buf);
            }
            else if (wmId == IDC_BTN_CONFIG) {
                DialogBoxParamW(GetModuleHandle(NULL), MAKEINTRESOURCEW(IDD_CONFIG), hwnd, ConfigDlgProc, 0);
            }
            else if (wmId == IDC_BTN_GET_ORCA) {
                // IDC_DAY で「全て」が選択されている時、UKEを読み込まなければ点検漏れがあるかもと警告
                if (SendMessageW(GetDlgItem(hwnd, IDC_DAY), CB_GETCURSEL, 0, 0) == 0) {
                    if (MessageBoxW(hwnd, L"ORCAの受付リストを使用して患者一覧を所得する場合、受付リストから漏れている患者の抽出ができません。\nこれは、誤操作で受付リストから削除した場合や、受付操作をしないで会計データを入力した場合に発生します。\n受付リストから漏れた患者も含めてチェックするには、「レセ電ファイルを用いて患者一覧を所得」を用いて、国保および社保のレセ電ファイルを読み込ませてください。", L"警告", MB_OKCANCEL | MB_ICONWARNING) != IDOK) {
                        return TRUE;
                    }
                }

                WCHAR wBuf[32];
                GetDlgItemTextW(hwnd, IDC_YEAR, wBuf, 32); int year = _wtoi(wBuf);
                GetDlgItemTextW(hwnd, IDC_MONTH, wBuf, 32); int month = _wtoi(wBuf);
                GetDlgItemTextW(hwnd, IDC_DAY, wBuf, 32); int day = (wcscmp(wBuf, L"全て") != 0) ? _wtoi(wBuf) : 0;

                ClearPatientData();
                StartFetchThread(hwnd, year, month, day);
            }
            else if (wmId == IDC_BTN_GET_BY_UKE) {
                WCHAR wBuf[32];
                GetDlgItemTextW(hwnd, IDC_YEAR, wBuf, 32); int year = _wtoi(wBuf);
                GetDlgItemTextW(hwnd, IDC_MONTH, wBuf, 32); int month = _wtoi(wBuf);
                GetDlgItemTextW(hwnd, IDC_DAY, wBuf, 32); int day = (wcscmp(wBuf, L"全て") != 0) ? _wtoi(wBuf) : 0;

                ClearPatientData();

                if (!SelectAndLoadUkeFile(hwnd, L"国保のレセ電ファイルを選択してください", year, month)) {
                    ClearPatientData();
                    return TRUE;
                }
                if (!SelectAndLoadUkeFile(hwnd, L"社保のレセ電ファイルを選択してください", year, month)) {
                    ClearPatientData();
                    return TRUE;
                }
                
                StartFetchThread(hwnd, year, month, day);
            }
            else if (wmId == IDCANCEL) {
                EndDialog(hwnd, 0);
            }
            return TRUE;
        }
    }
    return FALSE;
}

// ============================================================================
// プログレスウィンドウのプロシージャ
// ============================================================================
INT_PTR CALLBACK ProgressDlgProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
        case WM_INITDIALOG:
            PostMessage(GetDlgItem(hwnd, IDC_PROG_BAR), PBM_SETRANGE, 0, MAKELPARAM(0, 100));
            if (lParam) {
                HWND* phProgress = (HWND*)lParam;
                *phProgress = hwnd;
            }
            return TRUE;
        case WM_USER_PROGRESS:
            PostMessage(GetDlgItem(hwnd, IDC_PROG_BAR), PBM_SETPOS, (WPARAM)wParam, 0);
            return TRUE;
        case WM_USER_FINISHED:
            EndDialog(hwnd, (INT_PTR)wParam);
            return TRUE;
        case WM_COMMAND:
            if (LOWORD(wParam) == IDC_BTN_ABORT) {
                SetEvent(g_hStopEvent);
                SetDlgItemTextW(hwnd, IDC_ST_STATUS, L"中止待機中...");
                EnableWindow(GetDlgItem(hwnd, IDC_BTN_ABORT), FALSE);
            }
            return TRUE;
    }
    return FALSE;
}

// ============================================================================
// 設定ウィンドウのプロシージャ
// ============================================================================
INT_PTR CALLBACK ConfigDlgProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
        case WM_INITDIALOG: {
            LPWSTR wAddr = Utf8ToUtf16(g_settings.ORCAAddr);
            LPWSTR wUser = Utf8ToUtf16(g_settings.ORCAUser);
            LPWSTR wPass = Utf8ToUtf16(g_settings.ORCAPassword);
            LPWSTR wKey = Utf8ToUtf16(g_settings.GeminiKey);
            LPWSTR wModel = Utf8ToUtf16(g_settings.GeminiModel);
            LPWSTR wSysP = Utf8ToUtf16(g_settings.GEMSysPrompt);
            
            SetDlgItemTextW(hwnd, IDC_ED_ORCA_ADDR, wAddr);
            SetDlgItemTextW(hwnd, IDC_ED_ORCA_USER, wUser);
            SetDlgItemTextW(hwnd, IDC_ED_ORCA_PASS, wPass);
            SetDlgItemTextW(hwnd, IDC_ED_GEMINI_KEY, wKey);
            SetDlgItemTextW(hwnd, IDC_ED_GEMINI_MODEL, wModel);
            SetDlgItemTextW(hwnd, IDC_ED_SYS_PROMPT, wSysP);
            SetDlgItemInt(hwnd, IDC_ED_SPLIT_CASE, g_settings.GEMSplitCase, FALSE);
            CheckDlgButton(hwnd, IDC_CHK_DEBUG, g_settings.Debug ? BST_CHECKED : BST_UNCHECKED);
            CheckDlgButton(hwnd, IDC_CHK_PRIORITY, g_settings.Priority ? BST_CHECKED : BST_UNCHECKED);
            
            free(wAddr); free(wUser); free(wPass); free(wKey); free(wModel); free(wSysP);
            return TRUE;
        }
        case WM_COMMAND: {
            int id = LOWORD(wParam);
            if (id == IDOK) {
                WCHAR wBuf[2048];
                
                GetDlgItemTextW(hwnd, IDC_ED_ORCA_ADDR, wBuf, 2048);
                char* addr = Utf16ToUtf8(wBuf); strcpy(g_settings.ORCAAddr, addr); free(addr);
                
                GetDlgItemTextW(hwnd, IDC_ED_ORCA_USER, wBuf, 2048);
                char* user = Utf16ToUtf8(wBuf); strcpy(g_settings.ORCAUser, user); free(user);
                
                GetDlgItemTextW(hwnd, IDC_ED_ORCA_PASS, wBuf, 2048);
                char* pass = Utf16ToUtf8(wBuf); strcpy(g_settings.ORCAPassword, pass); free(pass);
                
                GetDlgItemTextW(hwnd, IDC_ED_GEMINI_KEY, wBuf, 2048);
                char* key = Utf16ToUtf8(wBuf); strcpy(g_settings.GeminiKey, key); free(key);
                
                GetDlgItemTextW(hwnd, IDC_ED_GEMINI_MODEL, wBuf, 2048);
                char* model = Utf16ToUtf8(wBuf); strcpy(g_settings.GeminiModel, model); free(model);
                
                GetDlgItemTextW(hwnd, IDC_ED_SYS_PROMPT, wBuf, 2048);
                char* sysp = Utf16ToUtf8(wBuf); strcpy(g_settings.GEMSysPrompt, sysp); free(sysp);
                
                g_settings.GEMSplitCase = GetDlgItemInt(hwnd, IDC_ED_SPLIT_CASE, NULL, FALSE);
                g_settings.Debug = (IsDlgButtonChecked(hwnd, IDC_CHK_DEBUG) == BST_CHECKED) ? 1 : 0;
                g_settings.Priority = (IsDlgButtonChecked(hwnd, IDC_CHK_PRIORITY) == BST_CHECKED) ? 1 : 0;
                
                FILE* fp = fopen("gemrecechk.ini", "w");
                if (fp) {
                    fprintf(fp, "orcaaddr=%s\n", g_settings.ORCAAddr);
                    fprintf(fp, "orcauser=%s\n", g_settings.ORCAUser);
                    fprintf(fp, "orcapassword=%s\n", g_settings.ORCAPassword);
                    fprintf(fp, "geminikey=%s\n", g_settings.GeminiKey);
                    fprintf(fp, "geminimodel=%s\n", g_settings.GeminiModel);
                    fprintf(fp, "gemsysprompt=%s\n", g_settings.GEMSysPrompt);
                    fprintf(fp, "gemsplitcase=%d\n", g_settings.GEMSplitCase);
                    fprintf(fp, "debug=%s\n", g_settings.Debug ? "yes" : "no");
                    fprintf(fp, "priority=%s\n", g_settings.Priority ? "yes" : "no");
                    fclose(fp);
                }
                
                EndDialog(hwnd, IDOK);
            }
            else if (id == IDCANCEL) {
                EndDialog(hwnd, IDCANCEL);
            }
            else if (id == IDC_BTN_EDIT_USERP) {
                SHELLEXECUTEINFOW sei = { sizeof(sei) };
                sei.fMask = SEE_MASK_NOCLOSEPROCESS;
                sei.lpVerb = L"open";
                sei.lpFile = L"notepad.exe";
                sei.lpParameters = L"gemprompt.txt";
                sei.nShow = SW_SHOWNORMAL;
                
                if (ShellExecuteExW(&sei)) {
                    EnableWindow(hwnd, FALSE); 
                    WaitForSingleObject(sei.hProcess, INFINITE); 
                    CloseHandle(sei.hProcess);
                    EnableWindow(hwnd, TRUE); 
                    SetForegroundWindow(hwnd);
                    
                    void LoadUserPrompt(); 
                    LoadUserPrompt();      
                }
            }
            return TRUE;
        }
    }
    return FALSE;
}

// ============================================================================
// レセプトビューアーで使う関数群
// ============================================================================

void UpdateFilteredList(HWND hwnd) {
    int ins_filter = (int)SendMessage(GetDlgItem(hwnd, IDC_COMBO_INS), CB_GETCURSEL, 0, 0);
    BOOL only_issue = (IsDlgButtonChecked(hwnd, IDC_CHK_ONLY_ISSUE) == BST_CHECKED);
    
    if (g_filtered_indices) free(g_filtered_indices);
    g_filtered_indices = (int*)malloc(sizeof(int) * g_patient_count);
    g_filtered_count = 0;

    for (int i = 0; i < g_patient_count; i++) {
        Patient_Data* p = &g_patients[i];
        if (ins_filter > 0 && p->Insurance_Flag != ins_filter) continue;
        if (only_issue) {
            if (!p->Gemini_Response || strstr(p->Gemini_Response, "OK") != NULL) continue;
        }
        g_filtered_indices[g_filtered_count++] = i;
    }
}

void DisplayPatient(HWND hwnd) {
    if (g_filtered_count == 0 || g_current_view_idx < 0 || g_current_view_idx >= g_filtered_count) {
        SetDlgItemTextW(hwnd, IDC_EDIT_CONTENT, L"表示するデータがありません。");
        return;
    }

    Patient_Data* p = &g_patients[g_filtered_indices[g_current_view_idx]];
    char buffer[65536] = {0};
    char temp[512];

    const char* ins_str = "自費等";
    if (p->Insurance_Flag == INSURANCE_SHAHO) ins_str = "社保";
    else if (p->Insurance_Flag == INSURANCE_KOKUHO) ins_str = "国保";
    else if (p->Insurance_Flag == INSURANCE_KOUKI) ins_str = "後期";

    snprintf(temp, sizeof(temp), "%s %s %s %d歳 %s\r\n\r\n", 
             p->Patient_ID, p->WholeName, p->BirthDate, p->Age, ins_str);
    strcat(buffer, temp);

    if (p->Gemini_Response && strlen(p->Gemini_Response) > 0) {
        strcat(buffer, "【判定結果】\r\n");
        strcat(buffer, p->Gemini_Response);
        strcat(buffer, "\r\n\r\n");
    }

    if (p->Disease_List) {
    	strcat(buffer, "【病名】\r\n");
        strcat(buffer, p->Disease_List);
        strcat(buffer, "\r\n"); 
    }

    if (p->Medical_Info_List) {
        strcat(buffer, p->Medical_Info_List);
    }

    LPWSTR wBuf = Utf8ToUtf16(buffer);
    SetDlgItemTextW(hwnd, IDC_EDIT_CONTENT, wBuf);
    free(wBuf);

    WCHAR wTitle[128];
    swprintf(wTitle, 128, L"レセプトの表示と点検 - %d / %d 件目", g_current_view_idx + 1, g_filtered_count);
    SetWindowTextW(hwnd, wTitle);
}

// ============================================================================
// レセプトビューアーのプロシージャ
// ============================================================================
INT_PTR CALLBACK ViewerDlgProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
        case WM_INITDIALOG: {
        	LONG_PTR exStyle = GetWindowLongPtrW(hwnd, GWL_EXSTYLE);
            exStyle |= WS_EX_APPWINDOW;    
            exStyle &= ~WS_EX_TOOLWINDOW;  
            SetWindowLongPtrW(hwnd, GWL_EXSTYLE, exStyle);
            HWND hCombo = GetDlgItem(hwnd, IDC_COMBO_INS);
            SendMessageW(hCombo, CB_ADDSTRING, 0, (LPARAM)L"全て");
            SendMessageW(hCombo, CB_ADDSTRING, 0, (LPARAM)L"社保");
            SendMessageW(hCombo, CB_ADDSTRING, 0, (LPARAM)L"国保");
            SendMessageW(hCombo, CB_ADDSTRING, 0, (LPARAM)L"後期");
            SendMessageW(hCombo, CB_SETCURSEL, 0, 0);
            UpdateFilteredList(hwnd);
            DisplayPatient(hwnd);
        	RECT rc;
            GetClientRect(hwnd, &rc);
            SendMessage(hwnd, WM_SIZE, SIZE_RESTORED, MAKELPARAM(rc.right, rc.bottom));
            return TRUE;
        }
        case WM_SIZE: { 
            int w = LOWORD(lParam);
            int h = HIWORD(lParam);
            // ウィンドウのリサイズにあわせてテキストボックス（レセプト表示エリア）をリサイズする
            MoveWindow(GetDlgItem(hwnd, IDC_EDIT_CONTENT), 10, 45, w - 20, h - 80, TRUE);
            // ウィンドウのリサイズにあわせて下部のボタンを移動する
            int btnY = h - 30;
            MoveWindow(GetDlgItem(hwnd, IDC_BTN_CHECK_ONE), 10, btnY, 120, 20, TRUE);
            MoveWindow(GetDlgItem(hwnd, IDC_BTN_CHECK_ALL), 140, btnY, 150, 20, TRUE);
            MoveWindow(GetDlgItem(hwnd, IDC_BTN_ABORT), w - 70, btnY, 60, 20, TRUE);
            MoveWindow(GetDlgItem(hwnd, IDC_BTN_PRINT_SUMMARY), w - 160, btnY, 80, 20, TRUE);
            
            return TRUE;
        }
        case WM_COMMAND: {
            int id = LOWORD(wParam);
            int notify = HIWORD(wParam);

            // 検索ボックス内でのEnterキー(IDOK)を検索ボタン押下として扱う
            if (id == IDOK) {
                if (GetFocus() == GetDlgItem(hwnd, IDC_EDIT_SEARCH)) {
                    id = IDC_BTN_SEARCH; 
                } else {
                    return TRUE; 
                }
            }

        	if (id == IDC_BTN_NEXT && notify == BN_CLICKED) {
                if (g_current_view_idx < g_filtered_count - 1) {
                    g_current_view_idx++;
                    DisplayPatient(hwnd);
                }
            }
            else if (id == IDC_BTN_PREV && notify == BN_CLICKED) {
                if (g_current_view_idx > 0) {
                    g_current_view_idx--;
                    DisplayPatient(hwnd);
                }
            }
            else if (id == IDC_COMBO_INS || id == IDC_CHK_ONLY_ISSUE) {
                UpdateFilteredList(hwnd); g_current_view_idx = 0; DisplayPatient(hwnd);
            }
            else if (id == IDC_BTN_CHECK_ONE || id == IDC_BTN_CHECK_ALL) {
                if (g_filtered_count == 0) return TRUE;

                int target_count = (id == IDC_BTN_CHECK_ONE) ? 1 : g_filtered_count;
                int* target_indices = (int*)malloc(sizeof(int) * target_count);
                if (id == IDC_BTN_CHECK_ONE) {
                    target_indices[0] = g_filtered_indices[g_current_view_idx];
                } else {
                    for (int i = 0; i < g_filtered_count; i++) target_indices[i] = g_filtered_indices[i];
                }
                
                if (strlen(g_settings.GeminiKey) == 0) {
                    if (MessageBoxW(hwnd, L"APIキーが設定されていません。\nペイロード(JSON)をdebug.txtに出力しますか？", L"確認", MB_YESNO | MB_ICONWARNING) == IDYES) {
                        void DumpGeminiJSON(int* indices, int count); 
                        DumpGeminiJSON(target_indices, target_count); 
                        MessageBoxW(hwnd, L"debug.txtにJSONを出力しました。\nプロンプトの構成を確認してください。", L"待機中", MB_OK | MB_ICONINFORMATION);
                    }
                    free(target_indices);
                    return TRUE; 
                }

                ResetEvent(g_hStopEvent);
                GeminiThreadArgs* args = (GeminiThreadArgs*)malloc(sizeof(GeminiThreadArgs));
                args->hProgressDlg = NULL;
                args->target_count = target_count;
                args->target_indices = target_indices;

                HANDLE hThread = (HANDLE)_beginthreadex(NULL, 0, RunGeminiCheckThread, args, 0, NULL);
                if (hThread) CloseHandle(hThread);
                INT_PTR res = DialogBoxParamW(GetModuleHandle(NULL), MAKEINTRESOURCEW(IDD_PROGRESS), hwnd, ProgressDlgProc, (LPARAM)&args->hProgressDlg);

                if (res == 1) {
                    MessageBoxW(hwnd, L"点検を中止しました。", L"中止", MB_OK);
                } else {
                    MessageBoxW(hwnd, L"点検が完了しました。", L"完了", MB_OK);
                }

                UpdateFilteredList(hwnd); 
                DisplayPatient(hwnd);     
            }
            else if (id == IDC_BTN_PRINT_SUMMARY) {
                PrintSummaryList(hwnd, g_filtered_indices, g_filtered_count);
            }
            else if (id == IDC_BTN_ABORT || id == IDCANCEL) {
            	if (MessageBoxW(hwnd, L"レセプトデータを破棄しますか？", L"確認", MB_YESNO) == IDYES){
            	    g_current_view_idx = 0;
                    EndDialog(hwnd, 0);
            	}
            }
            else if (id == IDC_BTN_SEARCH) {
                WCHAR wBuf[32];
                GetDlgItemTextW(hwnd, IDC_EDIT_SEARCH, wBuf, 32); 
                
                if (wcslen(wBuf) > 0) {
                    int search_id = _wtoi(wBuf);
                    char target_id_str[16];
                    snprintf(target_id_str, sizeof(target_id_str), "%06d", search_id);

                    int found_idx = -1;
                    for (int i = 0; i < g_filtered_count; i++) {
                        Patient_Data* p = &g_patients[g_filtered_indices[i]];
                        if (strcmp(p->Patient_ID, target_id_str) == 0) {
                            found_idx = i;
                            break;
                        }
                    }

                    if (found_idx != -1) {
                        g_current_view_idx = found_idx;
                        DisplayPatient(hwnd);
                    } else {
                        MessageBoxW(hwnd, L"指定の患者番号が見つかりません", L"検索結果", MB_OK | MB_ICONINFORMATION);
                    }
                }
            }
            return TRUE;
        }
    }
    return FALSE;
}

// ============================================================================
// エントリーポイント
// ============================================================================
int APIENTRY wWinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, LPWSTR lpCmdLine, int nCmdShow) {
    InitCommonControls(); 
    LoadSettings();
    g_hStopEvent = CreateEventW(NULL, TRUE, FALSE, NULL);
    DialogBoxParamW(hInstance, MAKEINTRESOURCEW(IDD_MAIN), NULL, MainDlgProc, 0);
    
    if (g_hStopEvent) CloseHandle(g_hStopEvent);
    if (g_settings.Debug) FreeConsole();
    
    return 0;
}
