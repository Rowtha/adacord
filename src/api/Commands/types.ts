/*
 * Adacord, a Discord client mod
 * Copyright (c) 2025 Vendicated and contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

import { Command } from "@adacord/discord-types";
export { ApplicationCommandInputType, ApplicationCommandOptionType, ApplicationCommandType } from "@adacord/discord-types/enums";

export interface AdacordCommand extends Command {
    isAdacordCommand?: boolean;
}
