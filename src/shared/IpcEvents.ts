/*
 * Adacord, a modification for Discord's desktop app
 * Copyright (c) 2023 Vendicated and contributors
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
*/

export const enum IpcEvents {
    INIT_FILE_WATCHERS = "AdacordInitFileWatchers",

    OPEN_QUICKCSS = "AdacordOpenQuickCss",
    GET_QUICK_CSS = "AdacordGetQuickCss",
    SET_QUICK_CSS = "AdacordSetQuickCss",
    QUICK_CSS_UPDATE = "AdacordQuickCssUpdate",

    GET_SETTINGS = "AdacordGetSettings",
    SET_SETTINGS = "AdacordSetSettings",

    GET_THEMES_LIST = "AdacordGetThemesList",
    GET_THEME_DATA = "AdacordGetThemeData",
    GET_THEME_SYSTEM_VALUES = "AdacordGetThemeSystemValues",
    THEME_UPDATE = "AdacordThemeUpdate",

    OPEN_EXTERNAL = "AdacordOpenExternal",
    OPEN_THEMES_FOLDER = "AdacordOpenThemesFolder",
    OPEN_SETTINGS_FOLDER = "AdacordOpenSettingsFolder",

    GET_UPDATES = "AdacordGetUpdates",
    GET_REPO = "AdacordGetRepo",
    UPDATE = "AdacordUpdate",
    BUILD = "AdacordBuild",

    OPEN_MONACO_EDITOR = "AdacordOpenMonacoEditor",
    GET_MONACO_THEME = "AdacordGetMonacoTheme",

    GET_PLUGIN_IPC_METHOD_MAP = "AdacordGetPluginIpcMethodMap",

    CSP_IS_DOMAIN_ALLOWED = "AdacordCspIsDomainAllowed",
    CSP_REMOVE_OVERRIDE = "AdacordCspRemoveOverride",
    CSP_REQUEST_ADD_OVERRIDE = "AdacordCspRequestAddOverride",

    GET_RENDERER_CSS = "AdacordGetRendererCss",
    RENDERER_CSS_UPDATE = "AdacordRendererCssUpdate",
    PRELOAD_GET_RENDERER_JS = "AdacordPreloadGetRendererJs",

    SUPPORTS_WINDOWS_MATERIAL = "AdacordSupportsWindowsMaterial",
}
