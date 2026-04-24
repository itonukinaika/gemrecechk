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

#include "gemrecechk.h"
#include "resource.h"
#include <windows.h>
#include <commdlg.h>
#include <stdio.h>
#include <stdlib.h>

extern int g_patient_count;

// ============================================================================
// 一覧印刷処理 (2段組み: 左40% 病名・右60% 点検結果
// ============================================================================
void PrintSummaryList(HWND hwnd, int* filtered_indices, int filtered_count) {
    if (filtered_count == 0) {
        MessageBoxW(hwnd, L"印刷するデータがありません。", L"エラー", MB_OK | MB_ICONERROR);
        return;
    }

    if (IsDlgButtonChecked(hwnd, IDC_CHK_ONLY_ISSUE) != BST_CHECKED) {
        if (MessageBoxW(hwnd, L"不備が見つかっていないレセプトも含めて全件を一覧印刷しようとしています。よろしいですか？", 
                        L"印刷確認", MB_YESNO | MB_ICONQUESTION) == IDNO) {
            return;
        }
    }

    PRINTDLGW pd = {0};
    pd.lStructSize = sizeof(PRINTDLGW);
    pd.hwndOwner = hwnd;
    pd.Flags = PD_RETURNDC | PD_NOPAGENUMS | PD_NOSELECTION;

    if (!PrintDlgW(&pd)) return;

    HDC hdc = pd.hDC;
    DOCINFOW di = {0};
    di.cbSize = sizeof(DOCINFOW);
    di.lpszDocName = L"Gemini AIでレセプトを点検するツール";

    if (StartDocW(hdc, &di) <= 0) {
        MessageBoxW(hwnd, L"印刷の開始に失敗しました。", L"エラー", MB_OK | MB_ICONERROR);
        DeleteDC(hdc);
        return;
    }

    int dpiX = GetDeviceCaps(hdc, LOGPIXELSX);
    int dpiY = GetDeviceCaps(hdc, LOGPIXELSY);
    int paperWidth = GetDeviceCaps(hdc, PHYSICALWIDTH);
    int paperHeight = GetDeviceCaps(hdc, PHYSICALHEIGHT);
    
    int marginX = dpiX / 2; 
    int marginY = dpiY / 2;
    int bottomLimit = paperHeight - marginY;

    // 印刷可能領域とカラム幅の計算
    int printableWidth = paperWidth - 2 * marginX;
    int leftColWidth = (int)(printableWidth * 0.4); // 左側 40%
    // int rightColWidth = printableWidth - leftColWidth; // 右側 60%
    int gutter = marginX / 4; // 縦線とテキストの間の余白
    int verticalLineX = marginX + leftColWidth; // 縦線を引くX座標

    int fontHeight = -MulDiv(11, dpiY, 72); // 一覧印刷用に少し小さめのフォント(11pt)
    HFONT hFont = CreateFontW(fontHeight, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                              SHIFTJIS_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                              DEFAULT_QUALITY, FIXED_PITCH | FF_MODERN, L"MS Gothic");
    HFONT hOldFont = (HFONT)SelectObject(hdc, hFont);

    // 罫線用のペン設定
    HPEN hPen = CreatePen(PS_SOLID, 1, RGB(0, 0, 0));
    HPEN hOldPen = (HPEN)SelectObject(hdc, hPen);

    // 空行の高さを取得
    int emptyLineHeight = 0;
    {
        RECT r = {0, 0, 100, 100};
        DrawTextW(hdc, L" ", -1, &r, DT_CALCRECT);
        emptyLineHeight = r.bottom - r.top;
    }

    int yPos = marginY;
    int isPageStarted = 0;

    for (int i = 0; i < filtered_count; i++) {
        Patient_Data* p = &g_patients[filtered_indices[i]];
        
        // バッファの確保 (テキストが長くなることを考慮)
        char* leftBuffer = (char*)calloc(1, 1024 * 64);
        char* rightBuffer = (char*)calloc(1, 1024 * 64);
        char temp[512];

        // --- 左カラムのテキスト構築 ---
        const char* ins_str = "自費等";
        if (p->Insurance_Flag == INSURANCE_SHAHO) ins_str = "社保";
        else if (p->Insurance_Flag == INSURANCE_KOKUHO) ins_str = "国保";
        else if (p->Insurance_Flag == INSURANCE_KOUKI) ins_str = "後期";

        snprintf(temp, sizeof(temp), "%s %s %d歳 %s\r\n", 
                 p->Patient_ID, p->WholeName, p->Age, ins_str);
        strcat(leftBuffer, temp);
        strcat(leftBuffer, "【病名】\r\n");
        if (p->Disease_List) {
            strcat(leftBuffer, p->Disease_List);
        }

        // --- 右カラムのテキスト構築 ---
        strcat(rightBuffer, "【判定結果】\r\n");
        if (p->Gemini_Response && strlen(p->Gemini_Response) > 0) {
            strcat(rightBuffer, p->Gemini_Response);
        }

        LPWSTR wLeft = Utf8ToUtf16(leftBuffer);
        LPWSTR wRight = Utf8ToUtf16(rightBuffer);

        // --- 描画領域の高さを事前計算 ---
        RECT calcLeft = { marginX, yPos, verticalLineX - gutter, bottomLimit };
        RECT calcRight = { verticalLineX + gutter, yPos, paperWidth - marginX, bottomLimit };

        DrawTextW(hdc, wLeft, -1, &calcLeft, DT_LEFT | DT_WORDBREAK | DT_NOPREFIX | DT_CALCRECT);
        DrawTextW(hdc, wRight, -1, &calcRight, DT_LEFT | DT_WORDBREAK | DT_NOPREFIX | DT_CALCRECT);

        int leftHeight = calcLeft.bottom - calcLeft.top;
        int rightHeight = calcRight.bottom - calcRight.top;
        int maxTextHeight = (leftHeight > rightHeight) ? leftHeight : rightHeight;
        
        // ブロック全体の高さ (テキスト高さ + 上下のマージン)
        int blockHeight = maxTextHeight + emptyLineHeight; 

        // --- 改ページ判定 (このブロックを描画すると下端を超える場合は次ページへ) ---
        if (isPageStarted && (yPos + blockHeight > bottomLimit)) {
            EndPage(hdc);
            isPageStarted = 0;
        }

        if (!isPageStarted) {
            if (StartPage(hdc) <= 0) break;
            isPageStarted = 1;
            yPos = marginY;
        }

        // --- 実際のテキスト描画 ---
        RECT drawLeft = { marginX, yPos, verticalLineX - gutter, yPos + leftHeight };
        RECT drawRight = { verticalLineX + gutter, yPos, paperWidth - marginX, yPos + rightHeight };

        DrawTextW(hdc, wLeft, -1, &drawLeft, DT_LEFT | DT_WORDBREAK | DT_NOPREFIX);
        DrawTextW(hdc, wRight, -1, &drawRight, DT_LEFT | DT_WORDBREAK | DT_NOPREFIX);

        // --- 中央の縦線を描画 ---
        MoveToEx(hdc, verticalLineX, yPos, NULL);
        LineTo(hdc, verticalLineX, yPos + maxTextHeight + (emptyLineHeight / 2));

        yPos += maxTextHeight + (emptyLineHeight / 2);

        // --- 下部の水平線を描画 ---
        MoveToEx(hdc, marginX, yPos, NULL);
        LineTo(hdc, paperWidth - marginX, yPos);

        yPos += (emptyLineHeight / 2); // 次のデータとの間隔

        free(wLeft);
        free(wRight);
        free(leftBuffer);
        free(rightBuffer);
    }

    if (isPageStarted) {
        EndPage(hdc);
    }

    SelectObject(hdc, hOldPen);
    DeleteObject(hPen);
    EndDoc(hdc);
    SelectObject(hdc, hOldFont);
    DeleteObject(hFont);
    DeleteDC(hdc);
}