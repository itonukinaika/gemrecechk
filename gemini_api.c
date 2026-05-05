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

// ファイル名の通り、Gemini APIと通信する関数群

#include "gemrecechk.h"
#include <process.h>
#include <winhttp.h>
#include <stdio.h>
#include "cJSON.h"

// ============================================================================
// Gemini API 要求スキーマをJSONで構築する関数
// ============================================================================
cJSON* BuildResponseSchema() {
    cJSON* schema = cJSON_CreateObject();
    cJSON_AddStringToObject(schema, "type", "array");
    cJSON* items = cJSON_CreateObject();
    cJSON_AddStringToObject(items, "type", "object");
    cJSON* properties = cJSON_CreateObject();
    // temp_idを必ず返信するよう指定(送信時と同じもの)
    cJSON* temp_id = cJSON_CreateObject();
    cJSON_AddStringToObject(temp_id, "type", "integer");
    cJSON_AddItemToObject(properties, "temp_id", temp_id);
    // 念のためプロンプトと同じ内容をここでも指定(deficiencyに返答し、OKならOKと返答すること)
    cJSON* deficiency = cJSON_CreateObject();
    cJSON_AddStringToObject(deficiency, "type", "string");
    cJSON_AddStringToObject(deficiency, "description", "不備の指摘内容。不備がない場合はOKを設定。");
    cJSON_AddItemToObject(properties, "deficiency", deficiency);
    
    cJSON_AddItemToObject(items, "properties", properties);
    cJSON* required = cJSON_CreateArray();
    cJSON_AddItemToArray(required, cJSON_CreateString("temp_id"));
    cJSON_AddItemToArray(required, cJSON_CreateString("deficiency"));
    cJSON_AddItemToObject(items, "required", required);
    cJSON_AddItemToObject(schema, "items", items);
    
    return schema;
}

// ============================================================================
// 点検対象のレセプトデータをJSON配列文字列にする関数
// ============================================================================
char* BuildPatientsJsonString(int* indices, int count) {
    cJSON* j_arr = cJSON_CreateArray();
    
    for (int i = 0; i < count; i++) {
        Patient_Data* p = &g_patients[indices[i]];
        cJSON* j_item = cJSON_CreateObject();
        // 0から順にtemp_idを付けて送信
        cJSON_AddNumberToObject(j_item, "temp_id", i);
        // 各レセプトの病名一覧
        cJSON_AddStringToObject(j_item, "disease_list", p->Disease_List ? p->Disease_List : "");
        // 各レセプトの診療行為一覧
        cJSON_AddStringToObject(j_item, "medical_info", p->Medical_Info_List ? p->Medical_Info_List : "");
        cJSON_AddItemToArray(j_arr, j_item);
    }
    
    char* json_str = cJSON_PrintUnformatted(j_arr);
    cJSON_Delete(j_arr);
    
    return json_str;
}

// ============================================================================
// 上記を用いてGemini API ペイロード構築
// ============================================================================
char* BuildGeminiPayload(int* indices, int count) {
    cJSON* root = cJSON_CreateObject();
    // システムプロンプトとユーザープロンプトを結合して、Geminiのsystem_instructionとして送信する
    char* combined_instr = (char*)malloc(strlen(g_settings.GEMSysPrompt) + strlen(g_settings.GEMUserPrompt) + 10);
    sprintf(combined_instr, "%s\n\n%s", g_settings.GEMSysPrompt, g_settings.GEMUserPrompt);
    cJSON* sys_inst = cJSON_CreateObject();
    cJSON* sys_parts = cJSON_CreateArray();
    cJSON* sys_part = cJSON_CreateObject();
    cJSON_AddStringToObject(sys_part, "text", combined_instr);
    cJSON_AddItemToArray(sys_parts, sys_part);
    cJSON_AddItemToObject(sys_inst, "parts", sys_parts);
    cJSON_AddItemToObject(root, "system_instruction", sys_inst);
    free(combined_instr);
    
    // 各レセプトを配列にまとめる
    cJSON* contents = cJSON_CreateArray();
    cJSON* content = cJSON_CreateObject();
    cJSON_AddStringToObject(content, "role", "user");
    char* patients_data = BuildPatientsJsonString(indices, count);
    cJSON* con_parts = cJSON_CreateArray();
    cJSON* con_part = cJSON_CreateObject();
    cJSON_AddStringToObject(con_part, "text", patients_data); 
    cJSON_AddItemToArray(con_parts, con_part);
    cJSON_AddItemToObject(content, "parts", con_parts);
    cJSON_AddItemToArray(contents, content);
    cJSON_AddItemToObject(root, "contents", contents);
    free(patients_data);
    
    cJSON* gen_config = cJSON_CreateObject();
    cJSON_AddStringToObject(gen_config, "response_mime_type", "application/json");
    cJSON_AddItemToObject(gen_config, "response_schema", BuildResponseSchema());
    cJSON_AddItemToObject(root, "generationConfig", gen_config);
    
    char* payload = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    
    return payload;
}

// ============================================================================
// デバッグ用ダンプ関数
// ============================================================================
void DumpGeminiJSON(int* target_indices, int target_count) {
    if (!g_settings.Debug) return;
    char* raw_payload = BuildGeminiPayload(target_indices, target_count);
    cJSON* root = cJSON_Parse(raw_payload);
    char* pretty_payload = cJSON_Print(root);
    DebugPrintf("\n=== [DEBUG] Gemini API ペイロードの確認 ===\n%s\n========================================\n\n", pretty_payload);
    free(raw_payload); free(pretty_payload); cJSON_Delete(root);
}

// ============================================================================
// Gemini API と通信するメインの関数 (自動リトライ機能付き)
// ============================================================================
char* CallGeminiAPI(const char* payload) {
    int max_retries = 3;      // 最大試行回数 (初回1回 + リトライ2回)
    int retry_delay_ms = 2000; // リトライ前の待機時間 (ミリ秒)
    char* response_buffer = NULL;
    
    for (int attempt = 1; attempt <= max_retries; attempt++) {
        // プログレスウィンドウで中止ボタンが押されていないか確認
        if (WaitForSingleObject(g_hStopEvent, 0) == WAIT_OBJECT_0) break;
        
        HINTERNET hSession = WinHttpOpen(L"gemrecechk/1.0", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
        
        // WinHttpのIPv6 Fast Fallback オプションを有効化(オンライン資格確認との兼ね合いで、必須)
        DWORD dwFastFallback = 1;
        WinHttpSetOption(hSession, WINHTTP_OPTION_IPV6_FAST_FALLBACK, &dwFastFallback, sizeof(dwFastFallback));
        // Geminiの応答には時間がかかるので、WinHttpのタイムアウトを長めに設定 (受信は120秒)
        WinHttpSetTimeouts(hSession, 60000, 60000, 60000, 120000);
        
        HINTERNET hConnect = WinHttpConnect(hSession, GEMINI_HOST, INTERNET_DEFAULT_HTTPS_PORT, 0);
        
        WCHAR urlPath[1024];
        LPWSTR wModel = Utf8ToUtf16(g_settings.GeminiModel);
        LPWSTR wKey = Utf8ToUtf16(g_settings.GeminiKey);
        swprintf(urlPath, 1024, L"/v1beta/models/%s:generateContent?key=%s", wModel, wKey);
        free(wModel); free(wKey);
        
        HINTERNET hRequest = WinHttpOpenRequest(hConnect, L"POST", urlPath, NULL, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE);
        
        LPCWSTR header = L"Content-Type: application/json\r\n";
        BOOL bResults = WinHttpSendRequest(hRequest, header, -1, (LPVOID)payload, strlen(payload), strlen(payload), 0);
        
        if (bResults) {
            bResults = WinHttpReceiveResponse(hRequest, NULL);
            if (!bResults && g_settings.Debug) {
                DebugPrintf("[Attempt %d/%d] WinHttpReceiveResponse Failed. Error: %lu\n", attempt, max_retries, GetLastError());
            }
        } else {
            if (g_settings.Debug) {
                DebugPrintf("[Attempt %d/%d] WinHttpSendRequest Failed. Error: %lu\n", attempt, max_retries, GetLastError());
            }
        }
        
        if (bResults) {
            DWORD dwSize = 0, dwDownloaded = 0, totalSize = 0;
            response_buffer = (char*)malloc(1);
            response_buffer[0] = '\0';
            BOOL bReadSuccess = TRUE;
            
            do {
                dwSize = 0;
                if (!WinHttpQueryDataAvailable(hRequest, &dwSize)) {
                    bReadSuccess = FALSE;
                    break;
                }
                if (dwSize == 0) break;
                
                char* temp_buf = (char*)malloc(dwSize + 1);
                if (WinHttpReadData(hRequest, (LPVOID)temp_buf, dwSize, &dwDownloaded)) {
                    temp_buf[dwDownloaded] = '\0';
                    response_buffer = (char*)realloc(response_buffer, totalSize + dwDownloaded + 1);
                    memcpy(response_buffer + totalSize, temp_buf, dwDownloaded);
                    totalSize += dwDownloaded;
                    response_buffer[totalSize] = '\0';
                } else {
                    free(temp_buf);
                    bReadSuccess = FALSE;
                    break;
                }
                free(temp_buf);
                
                if (WaitForSingleObject(g_hStopEvent, 0) == WAIT_OBJECT_0) {
                    free(response_buffer); response_buffer = NULL; break;
                }
            } while (dwSize > 0);

            // 読み取り途中でエラーになった場合はバッファを破棄してリトライへ
            if (!bReadSuccess) {
                if (response_buffer) {
                    free(response_buffer);
                    response_buffer = NULL;
                }
                if (g_settings.Debug) DebugPrintf("[Attempt %d/%d] Failed while reading data.\n", attempt, max_retries);
            }
        }

        WinHttpCloseHandle(hRequest); 
        WinHttpCloseHandle(hConnect); 
        WinHttpCloseHandle(hSession);

        // データが正常に取得できていればループを抜けて終了
        if (response_buffer) {
            if (g_settings.Debug) {
                DebugPrintf("--- Gemini API Response ---\n%s\n", response_buffer);
            }
            break;
        }

        // 失敗した場合の待機処理 (最大試行回数に達していない場合のみ)
        if (attempt < max_retries) {
            if (g_settings.Debug) DebugPrintf("Retrying in %d ms...\n", retry_delay_ms);
            
            // リトライ待ち中にユーザーが中止ボタンを押したら即座に中断
            if (WaitForSingleObject(g_hStopEvent, retry_delay_ms) == WAIT_OBJECT_0) {
                break;
            }
        }
    }

    return response_buffer;
}

// ============================================================================
// Gemini APIへの疎通性を確認する関数
// ============================================================================
BOOL CheckGeminiConnection() {
    HINTERNET hSession = WinHttpOpen(L"gemrecechk/1.0", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!hSession) return FALSE;
    // 短めのタイムアウトで疎通性を確認する
    DWORD dwFastFallback = 1;
    WinHttpSetOption(hSession, WINHTTP_OPTION_IPV6_FAST_FALLBACK, &dwFastFallback, sizeof(dwFastFallback));
    WinHttpSetTimeouts(hSession, 5000, 5000, 5000, 5000);

    HINTERNET hConnect = WinHttpConnect(hSession, GEMINI_HOST, INTERNET_DEFAULT_HTTPS_PORT, 0);
    if (!hConnect) { WinHttpCloseHandle(hSession); return FALSE; }

    HINTERNET hRequest = WinHttpOpenRequest(hConnect, L"GET", L"/", NULL, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE);
    if (!hRequest) { WinHttpCloseHandle(hConnect); WinHttpCloseHandle(hSession); return FALSE; }

    BOOL bResult = WinHttpSendRequest(hRequest, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0);
    if (bResult) {
        bResult = WinHttpReceiveResponse(hRequest, NULL);
    }

    WinHttpCloseHandle(hRequest);
    WinHttpCloseHandle(hConnect);
    WinHttpCloseHandle(hSession);

    return bResult;
}

// ============================================================================
// スレッド本体(メインの処理)
// ============================================================================
unsigned __stdcall RunGeminiCheckThread(void* param) {
    GeminiThreadArgs* args = (GeminiThreadArgs*)param;
    
    // 進捗ダイアログの画面準備が完了するまで待機する（最大5秒）
    // タイムアウトを設けるべきなのかどうか、いまひとつ謎
    int wait_timeout = 0;
    while (args->hProgressDlg == NULL && wait_timeout < 500) {
        Sleep(10);
        wait_timeout++;
    }

    // まずはGemini APIサーバーへの疎通性を確認
    if (!CheckGeminiConnection()) {
        MessageBoxW(args->hProgressDlg, L"Gemini APIサーバーに接続できません。\nインターネット接続を確認してください。", L"接続エラー", MB_OK | MB_ICONERROR);
        goto THREAD_ABORT;
    }

    int split_case = g_settings.GEMSplitCase > 0 ? g_settings.GEMSplitCase : 10;
    DWORD start_time = GetTickCount();

    for (int start_idx = 0; start_idx < args->target_count; start_idx += split_case) {
        if (WaitForSingleObject(g_hStopEvent, 0) == WAIT_OBJECT_0) goto THREAD_ABORT;

        int current_chunk_size = args->target_count - start_idx;
        if (current_chunk_size > split_case) current_chunk_size = split_case;

        int percent = (int)(((float)start_idx / args->target_count) * 100);
        PostMessage(args->hProgressDlg, WM_USER_PROGRESS, percent, 0);

        DWORD current_time = GetTickCount();
        DWORD elapsed = current_time - start_time;
        int remain_sec = 0;
        if (start_idx > 0) {
            float avg_time = (float)elapsed / start_idx;
            remain_sec = (int)((avg_time * (args->target_count - start_idx)) / 1000.0f);
        }
        WCHAR status_text[256];
        wsprintfW(status_text, L"Geminiでレセプトを点検しています(%d件中%d-%d件目 予想残り時間 %d分 %d秒)", 
            args->target_count, start_idx + 1, start_idx + current_chunk_size, remain_sec / 60, remain_sec % 60);
        SetDlgItemTextW(args->hProgressDlg, IDC_ST_STATUS, status_text);

        int* chunk_indices = &args->target_indices[start_idx];
        
        // 未点検かつ保険適用データ(自費等以外)だけを送信する。自費レセプトを送信するとお金の無駄になる
        // 点検に途中で失敗したら、点検を再実行すればよい。保険別に点検したりするときも便利
        int* unchecked_indices = (int*)malloc(sizeof(int) * current_chunk_size);
        int unchecked_count = 0;
        
        for (int i = 0; i < current_chunk_size; i++) {
            Patient_Data* p = &g_patients[chunk_indices[i]];
            if ((p->Gemini_Response == NULL || strlen(p->Gemini_Response) == 0) && p->Insurance_Flag != 0) {
                unchecked_indices[unchecked_count++] = chunk_indices[i];
            }
        }

        // 送信しようとしている全データが点検済み、または自費等だった場合は、まとめてスキップ
        if (unchecked_count == 0) {
            free(unchecked_indices);
            continue; 
        }

        // 未点検のデータ（unchecked_count件）だけでペイロード構築
        char* payload = BuildGeminiPayload(unchecked_indices, unchecked_count);
        
        // 実際に送信するデータをデバッグ出力
        if (g_settings.Debug) {
            DebugPrintf("\n=== [DEBUG] 実際に送信しています ===\n%s\n========================================\n", payload);
        }
        
        char* response = CallGeminiAPI(payload);
        free(payload);

        if (response) {
            cJSON* root = cJSON_Parse(response);
            if (root) {
                // Gemini APIのエラーを簡易的に確認する
                cJSON* error_node = cJSON_GetObjectItem(root, "error");
                if (error_node) {
                    cJSON* code_node = cJSON_GetObjectItem(error_node, "code");
                    if (cJSON_IsNumber(code_node)) {
                        if (code_node->valueint == 503) {
                            MessageBoxW(args->hProgressDlg, L"該当モデルが高負荷のため、処理を実行できません。モデルを変更するか、しばらく待ってからやり直してください。", L"APIエラー", MB_OK | MB_ICONERROR);
                            cJSON_Delete(root);
                            free(response);
                            free(unchecked_indices);
                            goto THREAD_ABORT;
                        } else if (code_node->valueint == 429) {
                            MessageBoxW(args->hProgressDlg, L"レートリミットに到達しました。解除されるまで待つか、クレジットカードを登録して残高をチャージしてください。", L"APIエラー", MB_OK | MB_ICONERROR);
                            cJSON_Delete(root);
                            free(response);
                            free(unchecked_indices);
                            goto THREAD_ABORT;
                        }
                    }
                }
                // Geminiからの応答をパース、temp_idから患者番号に結びつけなおして、deficiencyをPatient_Data構造体に記録する
                cJSON* candidates = cJSON_GetObjectItem(root, "candidates");
                if (cJSON_IsArray(candidates) && cJSON_GetArraySize(candidates) > 0) {
                    cJSON* first_candidate = cJSON_GetArrayItem(candidates, 0);
                    cJSON* content = cJSON_GetObjectItem(first_candidate, "content");
                    cJSON* parts = cJSON_GetObjectItem(content, "parts");
                    if (cJSON_IsArray(parts) && cJSON_GetArraySize(parts) > 0) {
                        cJSON* first_part = cJSON_GetArrayItem(parts, 0);
                        cJSON* text_node = cJSON_GetObjectItem(first_part, "text");
                        
                        if (cJSON_IsString(text_node)) {
                            cJSON* result_array = cJSON_Parse(text_node->valuestring);
                            if (cJSON_IsArray(result_array)) {
                                int res_count = cJSON_GetArraySize(result_array);
                                for (int i = 0; i < res_count; i++) {
                                    cJSON* item = cJSON_GetArrayItem(result_array, i);
                                    cJSON* t_id = cJSON_GetObjectItem(item, "temp_id");
                                    cJSON* def = cJSON_GetObjectItem(item, "deficiency");
                                    
                                    if (cJSON_IsNumber(t_id) && cJSON_IsString(def)) {
                                        int id_val = t_id->valueint;
                                        if (id_val >= 0 && id_val < unchecked_count) {
                                            Patient_Data* p = &g_patients[unchecked_indices[id_val]];
                                            if (p->Gemini_Response) free(p->Gemini_Response);
                                            p->Gemini_Response = _strdup(def->valuestring);
                                        }
                                    }
                                }
                            }
                            if (result_array) cJSON_Delete(result_array);
                        }
                    }
                }
                cJSON_Delete(root);
            }
            free(response);
        }
        free(unchecked_indices);
    }

    PostMessage(args->hProgressDlg, WM_USER_FINISHED, 0, 0);
    free(args->target_indices); free(args); return 0;

THREAD_ABORT:
    PostMessage(args->hProgressDlg, WM_USER_FINISHED, 1, 0);
    free(args->target_indices); free(args); return 0;
}