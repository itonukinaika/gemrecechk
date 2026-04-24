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

#ifndef RESOURCE_H
#define RESOURCE_H

#define IDC_STATIC          -1

#define IDD_MAIN            101
#define IDD_PROGRESS        102
#define IDD_VIEWER          103
#define IDD_CONFIG          104
#define IDD_LICENSE         105

// Main Window
#define IDC_YEAR            1001
#define IDC_MONTH           1002
#define IDC_DAY             1003
#define IDC_BTN_TODAY       1004
#define IDC_BTN_GET_ORCA    1005
#define IDC_BTN_CONFIG      1006
#define IDC_BTN_GET_BY_UKE  1007

// Progress Dialog
#define IDC_PROG_BAR        1101
#define IDC_BTN_ABORT       1102
#define IDC_ST_STATUS       1103

// Viewer Window
#define IDC_BTN_PREV        1201
#define IDC_BTN_NEXT        1202
#define IDC_EDIT_SEARCH     1203
#define IDC_BTN_SEARCH      1204
#define IDC_COMBO_INS       1205
#define IDC_CHK_ONLY_ISSUE  1206
#define IDC_EDIT_CONTENT    1207
#define IDC_BTN_CHECK_ONE   1208
#define IDC_BTN_CHECK_ALL   1209
// IDC_BTN_PRINT (1210) は廃止
#define IDC_BTN_PRINT_SUMMARY 1211

// Config Dialog
#define IDC_ED_ORCA_ADDR    1301
#define IDC_ED_ORCA_USER    1302
#define IDC_ED_ORCA_PASS    1303
#define IDC_ED_GEMINI_KEY   1304
#define IDC_ED_GEMINI_MODEL 1305
#define IDC_ED_SYS_PROMPT   1306
#define IDC_ED_SPLIT_CASE   1307
#define IDC_BTN_EDIT_USERP  1308
#define IDC_CHK_DEBUG       1309

// License Dialog
#define IDC_LBL_LICENSE_TITLE 1401
#define IDC_TXT_LICENSE_TEXT  1402
#define IDC_BTN_AGREE         1403
#define IDC_BTN_DECLINE       1404

#endif