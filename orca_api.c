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

// WebORCAと通信する関数群

#include "gemrecechk.h"
#include <process.h>
#include <winhttp.h>
#include <wincrypt.h>
#include <cryptuiapi.h>

// WebORCAクラウド版向けのクライアント証明書コンテキスト
PCCERT_CONTEXT g_client_cert_context = NULL;

// ============================================================================
// ヘルパー関数群
// ============================================================================

const char* SafeXmlTxt(ezxml_t node, const char* child_name) {
    if (!node) return "";
    ezxml_t c = ezxml_child(node, child_name);
    return (c && c->txt) ? c->txt : "";
}

// InsuranceClassを、国保、社保、後期に分類する
int DetermineInsuranceClass(const char* class_code) {
    if (!class_code || strlen(class_code) == 0) return 0;
    int num = atoi(class_code);
    if (num == 12) return INSURANCE_SHAHO; // ORCAにならって、生活保護は社保に分類する
    if (num == 39) return INSURANCE_KOUKI;  
    if (strncmp(class_code, "06", 2) == 0 || strncmp(class_code, "07", 2) == 0) return INSURANCE_KOKUHO;
    if (num > 0 && num < 60) return INSURANCE_SHAHO;
    return 0; 
}

int CalculateAge(const char* birth_date, int target_year, int target_month) {
    if (!birth_date || strlen(birth_date) < 10) return 0;
    int b_year = atoi(birth_date);
    int b_month = atoi(birth_date + 5);
    int age = target_year - b_year;
    if (target_month < b_month) age--;
    return age < 0 ? 0 : age;
}

int IsMedicationClass(const char* class_code) {
    if (!class_code || strlen(class_code) == 0) return 0;
    int code = atoi(class_code);
    if ((code >= 210 && code <= 290) || (code >= 310 && code <= 330)) return 1;
    return 0;
}

int ComparePatients(const void* a, const void* b) {
    return strcmp(((Patient_Data*)a)->Patient_ID, ((Patient_Data*)b)->Patient_ID);
}

// ============================================================================
// WebORCA通信ヘルパー
// ============================================================================
char* CallWebOrcaAPI(const char* endpoint, const char* req_xml) {
    if (WaitForSingleObject(g_hStopEvent, 0) == WAIT_OBJECT_0) return NULL;

    if (g_settings.Debug) {
        DebugPrintf("--- API Request to %s ---\n%s\n", endpoint, req_xml ? req_xml : "(GET)");
    }

    URL_COMPONENTSW urlComp = {0};
    urlComp.dwStructSize = sizeof(urlComp);
    WCHAR hostName[256] = {0};
    WCHAR urlPath[1024] = {0};
    urlComp.lpszHostName = hostName;
    urlComp.dwHostNameLength = 256;
    urlComp.lpszUrlPath = urlPath;
    urlComp.dwUrlPathLength = 1024;

    char full_url[2048];
    snprintf(full_url, sizeof(full_url), "%s%s", g_settings.ORCAAddr, endpoint);
    LPWSTR wUrl = Utf8ToUtf16(full_url);
    WinHttpCrackUrl(wUrl, 0, 0, &urlComp);
    free(wUrl);

    HINTERNET hSession = WinHttpOpen(L"gemrecechk/1.0", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);

    // WinHttpのIPv6 Fast Fallback オプションを有効化(オンライン資格確認との兼ね合いで、必須)
    DWORD dwFastFallback = 1;
    WinHttpSetOption(hSession, WINHTTP_OPTION_IPV6_FAST_FALLBACK, &dwFastFallback, sizeof(dwFastFallback));
    
    HINTERNET hConnect = WinHttpConnect(hSession, hostName, urlComp.nPort, 0);
    
    LPCWSTR verb = req_xml ? L"POST" : L"GET";
    DWORD dwFlags = 0;
    if (urlComp.nScheme == INTERNET_SCHEME_HTTPS) {
        dwFlags |= WINHTTP_FLAG_SECURE;
    }
    HINTERNET hRequest = WinHttpOpenRequest(hConnect, verb, urlPath, NULL, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, dwFlags);

    // WebORCAクラウド版では、疎通性確認の時点でクライアント証明書が選択されている
	if (g_client_cert_context) {
        WinHttpSetOption(hRequest, WINHTTP_OPTION_CLIENT_CERT_CONTEXT, (LPVOID)g_client_cert_context, sizeof(CERT_CONTEXT));
    }

    LPWSTR wUser = Utf8ToUtf16(g_settings.ORCAUser);
    LPWSTR wPass = Utf8ToUtf16(g_settings.ORCAPassword);
    WinHttpSetCredentials(hRequest, WINHTTP_AUTH_TARGET_SERVER, WINHTTP_AUTH_SCHEME_BASIC, wUser, wPass, NULL);
    free(wUser);
    free(wPass);

    BOOL bResults = FALSE;
    if (req_xml) {
        int req_len = strlen(req_xml);
        bResults = WinHttpSendRequest(hRequest, WINHTTP_NO_ADDITIONAL_HEADERS, 0, (LPVOID)req_xml, req_len, req_len, 0);
    } else {
        bResults = WinHttpSendRequest(hRequest, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0);
    }

    char* response_buffer = NULL;
    if (bResults) bResults = WinHttpReceiveResponse(hRequest, NULL);
    if (bResults) {
        DWORD dwSize = 0, dwDownloaded = 0, totalSize = 0;
        response_buffer = (char*)malloc(1);
        response_buffer[0] = '\0';
        do {
            dwSize = 0;
            if (!WinHttpQueryDataAvailable(hRequest, &dwSize)) break;
            if (dwSize == 0) break;
            char* temp_buf = (char*)malloc(dwSize + 1);
            if (WinHttpReadData(hRequest, (LPVOID)temp_buf, dwSize, &dwDownloaded)) {
                temp_buf[dwDownloaded] = '\0';
                response_buffer = (char*)realloc(response_buffer, totalSize + dwDownloaded + 1);
                memcpy(response_buffer + totalSize, temp_buf, dwDownloaded);
                totalSize += dwDownloaded;
                response_buffer[totalSize] = '\0';
            }
            free(temp_buf);
        } while (dwSize > 0);
    }

    WinHttpCloseHandle(hRequest); WinHttpCloseHandle(hConnect); WinHttpCloseHandle(hSession);
    if (g_settings.Debug && response_buffer) DebugPrintf("--- API Response ---\n%s\n", response_buffer);
    return response_buffer;
}

// ============================================================================
// パース・整形ロジック
// ============================================================================

// 病名一覧の作成
void ProcessDiseaseInfo(Patient_Data* p, int target_year, int target_month) {
    char req_xml[512];
    snprintf(req_xml, sizeof(req_xml),
        "<data>\n  <disease_inforeq type=\"record\">\n    <Patient_ID type=\"string\">%s</Patient_ID>\n    <Base_Date type=\"string\">%04d-%02d</Base_Date>\n  </disease_inforeq>\n</data>",
        p->Patient_ID, target_year, target_month);

    char* res_xml = CallWebOrcaAPI("diseasegetv2?class=01", req_xml);
    if (!res_xml) return;

    ezxml_t doc = ezxml_parse_str(res_xml, strlen(res_xml));
    if (doc) {
        ezxml_t res_node = ezxml_child(doc, "disease_infores");
        ezxml_t disease_info = res_node ? ezxml_child(res_node, "Disease_Information") : NULL;
        if (disease_info) {
            char buffer[4096] = "";
            int count = 1;
            for (ezxml_t node = ezxml_child(disease_info, "Disease_Information_child"); node; node = node->next) {
                const char* name = SafeXmlTxt(node, "Disease_Name");
                const char* start = SafeXmlTxt(node, "Disease_StartDate");
                const char* category = SafeXmlTxt(node, "Disease_Category");
                const char* outcome = SafeXmlTxt(node, "Disease_OutCome");
                
                const char* suspect = SafeXmlTxt(node, "Disease_SuspectedFlag");
                char suspicion[16] = "";
                if (suspect && strchr(suspect, 'S') != NULL) {
                    if (strstr(name, "疑い") == NULL) {
                        strcpy(suspicion, "の疑い");
                    }
                }

                char main_flag[16] = "";
                if (category && strcmp(category, "PD") == 0) strcpy(main_flag, "(主)");
                
                char outcome_str[16] = "";
                if (outcome && strlen(outcome) > 0) {
                    if (strcmp(outcome, "F") == 0) strcpy(outcome_str, " 治癒");
                    else if (strcmp(outcome, "D") == 0) strcpy(outcome_str, " 死亡");
                    else if (strcmp(outcome, "C") == 0) strcpy(outcome_str, " 中止");
                    else if (strcmp(outcome, "S") == 0) strcpy(outcome_str, " 移行");
                }

                char line[256];
                snprintf(line, sizeof(line), "(%d) %s%s%s %s%s\r\n", count++, name, suspicion, main_flag, start, outcome_str);
                strncat(buffer, line, sizeof(buffer) - strlen(buffer) - 1);
            }
            p->Disease_List = _strdup(buffer);
        }
        ezxml_free(doc);
    }
    free(res_xml);
}

void AddPatientDeduplicated(const char* pid, const char* name, const char* dob, int target_year, int target_month) {
    for (int i = 0; i < g_patient_count; i++) {
        if (strcmp(g_patients[i].Patient_ID, pid) == 0) return;
    }
    g_patients = (Patient_Data*)realloc(g_patients, sizeof(Patient_Data) * (g_patient_count + 1));
    Patient_Data* p = &g_patients[g_patient_count];
    memset(p, 0, sizeof(Patient_Data));
    strncpy(p->Patient_ID, pid, sizeof(p->Patient_ID) - 1);
    strncpy(p->WholeName, name, sizeof(p->WholeName) - 1);
    strncpy(p->BirthDate, dob, sizeof(p->BirthDate) - 1);
    p->Age = CalculateAge(dob, target_year, target_month);
    g_patient_count++;
}

Medical_Class_Info* SortMedicalClassList(Medical_Class_Info* head) {
    if (!head || !head->next) return head;
    int count = 0;
    for (Medical_Class_Info* p = head; p; p = p->next) count++;
    Medical_Class_Info** arr = (Medical_Class_Info**)malloc(sizeof(Medical_Class_Info*) * count);
    int idx = 0;
    for (Medical_Class_Info* p = head; p; p = p->next) arr[idx++] = p;
    for (int i = 0; i < count - 1; i++) {
        for (int j = i + 1; j < count; j++) {
            if (strcmp(arr[i]->Medical_Class, arr[j]->Medical_Class) > 0) {
                Medical_Class_Info* tmp = arr[i]; arr[i] = arr[j]; arr[j] = tmp;
            }
        }
    }
    for (int i = 0; i < count - 1; i++) arr[i]->next = arr[i + 1];
    arr[count - 1]->next = NULL;
    head = arr[0]; free(arr);
    return head;
}

void FreeMedicalClassList(Medical_Class_Info* head) {
    while (head) {
        Medication_Info* m_head = head->details_head;
        while (m_head) { Medication_Info* m_next = m_head->next; free(m_head); m_head = m_next; }
        Medical_Class_Info* next = head->next; free(head); head = next;
    }
}

// ============================================================================
// WebORCAへの疎通性を確認する関数
// ============================================================================
int CheckWebOrcaConnection(HWND hwndParent) {
    URL_COMPONENTSW urlComp = {0};
    urlComp.dwStructSize = sizeof(urlComp);
    WCHAR hostName[256] = {0};
    WCHAR urlPath[1024] = {0};
    urlComp.lpszHostName = hostName;
    urlComp.dwHostNameLength = 256;
    urlComp.lpszUrlPath = urlPath;
    urlComp.dwUrlPathLength = 1024;

    if (g_settings.ORCAAddr[0] == '\0') return 1;

    LPWSTR wUrl = Utf8ToUtf16(g_settings.ORCAAddr);
    if (!WinHttpCrackUrl(wUrl, 0, 0, &urlComp)) {
        free(wUrl);
        return 1;
    }
    free(wUrl);

	HINTERNET hSession = WinHttpOpen(L"gemrecechk/1.0", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!hSession) return 1;

    DWORD dwFastFallback = 1;
    WinHttpSetOption(hSession, WINHTTP_OPTION_IPV6_FAST_FALLBACK, &dwFastFallback, sizeof(dwFastFallback));
    WinHttpSetTimeouts(hSession, 5000, 5000, 5000, 5000);

    HINTERNET hConnect = WinHttpConnect(hSession, hostName, urlComp.nPort, 0);
    if (!hConnect) { WinHttpCloseHandle(hSession); return 1; }

    LPCWSTR reqPath = (wcslen(urlPath) > 0) ? urlPath : L"/";
    DWORD dwFlags = 0;
    if (urlComp.nScheme == INTERNET_SCHEME_HTTPS) {
        dwFlags |= WINHTTP_FLAG_SECURE;
    }
    HINTERNET hRequest = WinHttpOpenRequest(hConnect, L"GET", reqPath, NULL, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, dwFlags);
    if (!hRequest) { WinHttpCloseHandle(hConnect); WinHttpCloseHandle(hSession); return 1; }

    // 既にクライアント証明書がセットされていれば、これを用いて接続する
	if (g_client_cert_context) {
        WinHttpSetOption(hRequest, WINHTTP_OPTION_CLIENT_CERT_CONTEXT, (LPVOID)g_client_cert_context, sizeof(CERT_CONTEXT));
    }

    LPWSTR wUser = Utf8ToUtf16(g_settings.ORCAUser);
    LPWSTR wPass = Utf8ToUtf16(g_settings.ORCAPassword);
    WinHttpSetCredentials(hRequest, WINHTTP_AUTH_TARGET_SERVER, WINHTTP_AUTH_SCHEME_BASIC, wUser, wPass, NULL);
    free(wUser);
    free(wPass);

    BOOL bResult = WinHttpSendRequest(hRequest, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0);
    if (bResult) {
        bResult = WinHttpReceiveResponse(hRequest, NULL);
    }
    
	// 接続先がWebORCAクラウド版で、クライアント証明書が要求された場合は、Windowsの証明書ストアから選択させる
    if (!bResult && GetLastError() == ERROR_WINHTTP_CLIENT_AUTH_CERT_NEEDED) {
        HCERTSTORE hMyStore = CertOpenSystemStoreW(0, L"MY");
        if (hMyStore) {
            PCCERT_CONTEXT pCertContext = CryptUIDlgSelectCertificateFromStore(
                hMyStore,
                hwndParent,
                L"クライアント証明書の選択",
                L"WebORCAクラウド版に接続するための証明書を選択してください。",
                CRYPTUI_SELECT_LOCATION_COLUMN,
                0,
                NULL
            );
            if (pCertContext) {
                if (g_client_cert_context) {
                    CertFreeCertificateContext(g_client_cert_context);
                }
                g_client_cert_context = CertDuplicateCertificateContext(pCertContext);
                
                WinHttpSetOption(hRequest, WINHTTP_OPTION_CLIENT_CERT_CONTEXT, (LPVOID)pCertContext, sizeof(CERT_CONTEXT));
                CertFreeCertificateContext(pCertContext);
                
                bResult = WinHttpSendRequest(hRequest, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0);
                if (bResult) {
                    bResult = WinHttpReceiveResponse(hRequest, NULL);
                }
            }
            CertCloseStore(hMyStore, 0);
        }
    }

    int status = 1; // デフォルトは接続エラー(1)
    if (bResult) {
            DWORD dwStatusCode = 0;
            DWORD dwSize = sizeof(dwStatusCode);
            WinHttpQueryHeaders(hRequest, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX, &dwStatusCode, &dwSize, WINHTTP_NO_HEADER_INDEX);
            if (dwStatusCode == 401) {
                status = 2; // 認証エラー(401)
            } else {
                status = 0; // 成功(接続完了かつ認証通過)
            }
        }

    WinHttpCloseHandle(hRequest);
    WinHttpCloseHandle(hConnect);
    WinHttpCloseHandle(hSession);

    return status;
}

// ============================================================================
// スレッド本体
// ============================================================================
unsigned __stdcall FetchOrcaDataThread(void* param) {
    OrcaThreadArgs* args = (OrcaThreadArgs*)param;
    char req_xml[1024];
    int start_day = (args->target_day == 0) ? 1 : args->target_day;
    int end_day = (args->target_day == 0) ? 31 : args->target_day;

    // 進捗ダイアログの画面準備が完了するまで5秒待つ。5秒の根拠はない。
    int wait_timeout = 0;
    while (args->hProgressDlg == NULL && wait_timeout < 500) {
        Sleep(10);
        wait_timeout++;
    }

    // WebORCAへの疎通性チェックとBASIC認証チェック
    int conn_status = CheckWebOrcaConnection(args->hProgressDlg);
    if (conn_status == 1) {
        MessageBoxW(args->hProgressDlg, L"WebORCAに接続できません。\n設定されているアドレスが正しいか、証明書が正しく選択されているか等を確認してください。", L"接続エラー", MB_OK | MB_ICONERROR);
        goto THREAD_ABORT;
    } else if (conn_status == 2) {
        MessageBoxW(args->hProgressDlg, L"WebORCAの認証に失敗しました。\nユーザー名とパスワードが正しいか確認してください。", L"認証エラー", MB_OK | MB_ICONERROR);
        goto THREAD_ABORT;
    }

    // 受付リストを走査して患者一覧をリストアップ
	// UKEファイルから患者一覧が既に読み込まれている場合は、WebORCA APIからの取得をスキップ
    if (g_patient_count == 0) {
        for (int d = start_day; d <= end_day; d++) {
            if (WaitForSingleObject(g_hStopEvent, 0) == WAIT_OBJECT_0) goto THREAD_ABORT;
            snprintf(req_xml, sizeof(req_xml),
                "<data>\n  <acceptlstreq type=\"record\">\n    <Acceptance_Date type=\"string\">%04d-%02d-%02d</Acceptance_Date>\n  </acceptlstreq>\n</data>",
                args->target_year, args->target_month, d);
            char* res_xml = CallWebOrcaAPI("acceptlstv2?class=02", req_xml);
            if (res_xml) {
                ezxml_t doc = ezxml_parse_str(res_xml, strlen(res_xml));
                if (doc) {
                    ezxml_t res_node = ezxml_child(doc, "acceptlstres");
                    ezxml_t accept_info = res_node ? ezxml_child(res_node, "Acceptlst_Information") : NULL;
                    for (ezxml_t child = accept_info ? ezxml_child(accept_info, "Acceptlst_Information_child") : NULL; child; child = child->next) {
                        ezxml_t pat_info = ezxml_child(child, "Patient_Information");
                        if (pat_info) {
                            AddPatientDeduplicated(SafeXmlTxt(pat_info, "Patient_ID"), SafeXmlTxt(pat_info, "WholeName"), SafeXmlTxt(pat_info, "BirthDate"), args->target_year, args->target_month);
                        }
                    }
                    ezxml_free(doc);
                }
                free(res_xml);
            }
        }
    }

    if (g_patient_count == 0) goto THREAD_END;

    // 患者番号順に並べ替える
	qsort(g_patients, g_patient_count, sizeof(Patient_Data), ComparePatients);

    for (int i = 0; i < g_patient_count; i++) {
        if (WaitForSingleObject(g_hStopEvent, 0) == WAIT_OBJECT_0) goto THREAD_ABORT;
        Patient_Data* p = &g_patients[i];
        PostMessage(args->hProgressDlg, WM_USER_PROGRESS, (int)(((float)i / g_patient_count) * 100), 0);

        // 各患者番号に対して、その月の受付一覧(Perform_Date, Department_Code, Sequential_Numberの組の一覧)を所得する
    	snprintf(req_xml, sizeof(req_xml),
            "<data>\n  <medicalgetreq type=\"record\">\n    <Patient_ID type=\"string\">%s</Patient_ID>\n    <Perform_Date type=\"string\">%04d-%02d-01</Perform_Date>\n  </medicalgetreq>\n</data>",
            p->Patient_ID, args->target_year, args->target_month);
        
        char* get_res = CallWebOrcaAPI("medicalgetv2?class=01", req_xml);
        if (!get_res) continue;

        Medical_Class_Info* class_list_head = NULL;
        ezxml_t get_doc = ezxml_parse_str(get_res, strlen(get_res));
        if (get_doc) {
            ezxml_t res_node = ezxml_child(get_doc, "medicalget01res");
            ezxml_t lst = res_node ? ezxml_child(res_node, "Medical_List_Information") : NULL;
        	// 各会計番号Sequential_Numberに対して、保険分類の確認と、診療内容の所得を行う
            for (ezxml_t child = lst ? ezxml_child(lst, "Medical_List_Information_child") : NULL; child; child = child->next) {
                
                int ins_flag = 0;
                ezxml_t h_info = ezxml_child(child, "HealthInsurance_Information");
                // InsuranceProvider_Classを見て、国保社保後期を分類する
                if (h_info) {
                    ins_flag = DetermineInsuranceClass(SafeXmlTxt(h_info, "InsuranceProvider_Class"));
                    // InsuranceProvider_Classが無い場合は、自費か公費のみである。生活保護だと公費のみが登録されており、これを検出する必要がある。
                	// 面倒なので、PublicInsurance_Classがあれば、代わりにこちらの値で判定する仕様とした
                    if (ins_flag == 0) {
                        ezxml_t pub_info = ezxml_child(h_info, "PublicInsurance_Information");
                        
                        if (pub_info) {
                            ezxml_t pub_child = ezxml_child(pub_info, "PublicInsurance_Information_child");
                            for (; pub_child; pub_child = pub_child->next) {
                                int flag = DetermineInsuranceClass(SafeXmlTxt(pub_child, "PublicInsurance_Class"));
                                if (flag != 0) {
                                    ins_flag = flag;
                                    break;
                                }
                            }
                        }
                    }
                }

                if (ins_flag > 0) {
                    p->Insurance_Flag = ins_flag;
                	// 上の判定で保険か生活保護であれば、診療内容を所得する
                	// ここの入れ子構造のパースが複雑で、スパゲッティにする自信があったので、geminiに書いてもらった
                    snprintf(req_xml, sizeof(req_xml),
                        "<data>\n  <medicalgetreq type=\"record\">\n    <Patient_ID type=\"string\">%s</Patient_ID>\n    <Perform_Date type=\"string\">%s</Perform_Date>\n    <Medical_Information type=\"record\">\n      <Department_Code type=\"string\">%s</Department_Code>\n      <Sequential_Number type=\"string\">%s</Sequential_Number>\n    </Medical_Information>\n  </medicalgetreq>\n</data>",
                        p->Patient_ID, SafeXmlTxt(child, "Perform_Date"), SafeXmlTxt(child, "Department_Code"), SafeXmlTxt(child, "Sequential_Number"));
                    char* info_res = CallWebOrcaAPI("medicalgetv2?class=02", req_xml);
                    if (info_res) {
                        ezxml_t info_doc = ezxml_parse_str(info_res, strlen(info_res));
                        if (info_doc) {
                            ezxml_t r02 = ezxml_child(info_doc, "medicalget02res");
                            ezxml_t li = r02 ? ezxml_child(r02, "Medical_List_Information") : NULL;
                            ezxml_t lc = li ? ezxml_child(li, "Medical_List_Information_child") : NULL;
                            ezxml_t mi_node = lc ? ezxml_child(lc, "Medical_Information") : NULL;
                            for (ezxml_t m_child = mi_node ? ezxml_child(mi_node, "Medical_Information_child") : NULL; m_child; m_child = m_child->next) {
                                Medical_Class_Info* mci = (Medical_Class_Info*)calloc(1, sizeof(Medical_Class_Info));
                                strncpy(mci->Medical_Class, SafeXmlTxt(m_child, "Medical_Class"), 15);
                                snprintf(mci->Medical_Class_Name, 63, "【%s】", SafeXmlTxt(m_child, "Medical_Class_Name"));
                                strncpy(mci->Medical_Class_Number, SafeXmlTxt(m_child, "Medical_Class_Number"), 31);
                                strncpy(mci->Medical_Class_Point, SafeXmlTxt(m_child, "Medical_Class_Point"), 31);
                                mci->is_medication = IsMedicationClass(mci->Medical_Class);
                                Medication_Info* last_med = NULL;
                                ezxml_t meds = ezxml_child(m_child, "Medication_info");
                                for (ezxml_t med_c = meds ? ezxml_child(meds, "Medication_info_child") : NULL; med_c; med_c = med_c->next) {
                                    Medication_Info* mi = (Medication_Info*)calloc(1, sizeof(Medication_Info));
                                    strncpy(mi->Medication_Name, SafeXmlTxt(med_c, "Medication_Name"), 255);
                                    strncpy(mi->Medication_Name_Input_Value, SafeXmlTxt(med_c, "Medication_Name_Input_Value"), 255);
                                    strncpy(mi->Medication_Number, SafeXmlTxt(med_c, "Medication_Number"), 31);
                                    strncpy(mi->Unit_Code_Name, SafeXmlTxt(med_c, "Unit_Code_Name"), 31);
                                    strncpy(mi->Medication_Point, SafeXmlTxt(med_c, "Medication_Point"), 31);
                                	strncpy(mi->Medication_Code, SafeXmlTxt(med_c, "Medication_Code"), 31); // コメント識別
                                    if (!mci->details_head) mci->details_head = mi; else last_med->next = mi;
                                    last_med = mi;
                                }
                                mci->next = class_list_head; class_list_head = mci;
                            }
                            ezxml_free(info_doc);
                        }
                        free(info_res);
                    }
                }
            }
            ezxml_free(get_doc);
        }
        free(get_res);

        class_list_head = SortMedicalClassList(class_list_head);
        char buffer[32768] = ""; char last_class[16] = "";
    	// 調剤料や処方料はORCAが自動で算出するものなので、点検しない 
        for (Medical_Class_Info* mci = class_list_head; mci; mci = mci->next) {
            if (strcmp(mci->Medical_Class_Name, "【内服調剤料】") == 0 ||
                strcmp(mci->Medical_Class_Name, "【外用調剤料】") == 0 ||
                strcmp(mci->Medical_Class_Name, "【処方料】") == 0 ||
        	    strcmp(mci->Medical_Class_Name, "【保険外（消費税なし）】") == 0) {
                continue;
            }
            if (strcmp(last_class, mci->Medical_Class) != 0) {
                if (strlen(last_class) > 0) strcat(buffer, "\r\n"); 
                strcat(buffer, mci->Medical_Class_Name); strcat(buffer, "\r\n");
                strcpy(last_class, mci->Medical_Class);
            }
            if (mci->is_medication) {
                for (Medication_Info* mi = mci->details_head; mi; mi = mi->next) {
                    char line[512] = {0};
                    if (strlen(mi->Unit_Code_Name) > 0) {
                        // 「約」の表示に意味が無いので削除
                        snprintf(line, sizeof(line), "%s(%s円) %s%s\r\n", mi->Medication_Name, mi->Medication_Point, mi->Medication_Number, mi->Unit_Code_Name);
                    } else {
                        char tmp_name[512]; strncpy(tmp_name, mi->Medication_Name, 511);
                        
                        // Medication_Code が "8" で始まる場合をコメント扱い
                        if (strncmp(mi->Medication_Code, "8", 1) == 0) {
                            snprintf(line, sizeof(line), "コメント：%s%s\r\n", tmp_name, mi->Medication_Name_Input_Value);
                        } else if (strncmp(tmp_name, "【", 3) == 0) {
                            char* end = tmp_name + strlen(tmp_name) - 3;
                            if (strcmp(end, "】") == 0) {
                                *end = '\0';
                                snprintf(line, sizeof(line), "(%s)%s\r\n", tmp_name + 3, mi->Medication_Name_Input_Value);
                            } else { snprintf(line, sizeof(line), "(%s)%s\r\n", tmp_name, mi->Medication_Name_Input_Value); }
                        } else { 
                            snprintf(line, sizeof(line), "(%s)%s\r\n", tmp_name, mi->Medication_Name_Input_Value); 
                        }
                    }
                    strcat(buffer, line);
                }
                int total_point = atoi(mci->Medical_Class_Point) * atoi(mci->Medical_Class_Number);
                // 合計の下に空行を入れて、見やすくした
                char sum_line[128]; snprintf(sum_line, sizeof(sum_line), "x %s 合計 %d 点\r\n\r\n", mci->Medical_Class_Number, total_point);
                strcat(buffer, sum_line);
            } else {
                for (Medication_Info* mi = mci->details_head; mi; mi = mi->next) {
                    if (strlen(mi->Medication_Name) > 0) {
                        char line[512]; 
                        // 検査や処置等のブロックでもコメントを判別
                        if (strncmp(mi->Medication_Code, "8", 1) == 0) {
                            snprintf(line, sizeof(line), "コメント：%s%s\r\n", mi->Medication_Name, mi->Medication_Name_Input_Value);
                        } else {
                            snprintf(line, sizeof(line), "%s %s x %s\r\n", mi->Medication_Name, mi->Medication_Point, mci->Medical_Class_Number);
                        }
                        strcat(buffer, line);
                    }
                }
            }
        }
        if (strlen(buffer) > 0) p->Medical_Info_List = _strdup(buffer);
        FreeMedicalClassList(class_list_head);
        ProcessDiseaseInfo(p, args->target_year, args->target_month);
    }

THREAD_END:
    PostMessage(args->hProgressDlg, WM_USER_FINISHED, 0, 0); free(args); return 0;
THREAD_ABORT:
    PostMessage(args->hProgressDlg, WM_USER_FINISHED, 1, 0); free(args); return 0;
}