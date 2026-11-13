// coop/config/config_registry_rows_dev_chat.inc -- the [dev] rows of the chat and interface-language drills: the chat drill, the
// l10n drill and the chat drill's red arms, moved in their order. Included by config_registry_rows_dev.inc where they stood,
// so each row keeps its place in every expansion of the list. The entry forms are config_registry_rows.inc's.

CFG_ENUM(chat_drill, "chat_drill", "dev", "off", "off|history|seed|i18n|span", "VOTVCOOP_CHAT_DRILL",
    "Developer: chat checks judged in memory -- history (the host), seed (the second joiner), i18n "
    "(every peer), span (every peer: a mid-line nick's span through the feed); each prints "
    "[CHAT-DRILL] PASS, FAIL or ABORT.")
CFG_ENUM(l10n_drill, "l10n_drill", "dev", "off", "off|translated|english|atlas", "VOTVCOOP_L10N_DRILL",
    "Developer: the interface-language drill on each peer -- opens two F1 panes, waits for the join line, "
    "then expects them translated or English; atlas floods the chat with ideographs under the Chinese pack "
    "and measures the font atlas; prints [L10N-DRILL] DONE or FAIL.")
CFG_FLAG(chat_span_prefix, "chat_span_prefix", "dev", false, "VOTVCOOP_CHAT_SPAN_PREFIX",
    "Developer: keep a peer action's nick as a prefix wherever its sentence put it (the chat drill's span red).")
CFG_FLAG(chat_drill_bad_nick, "chat_drill_bad_nick", "dev", false, "VOTVCOOP_CHAT_DRILL_BAD_NICK",
    "Developer: the chat drill announces a wrong nickname (the i18n red).")
CFG_FLAG(chat_no_retain, "chat_no_retain", "dev", false, "VOTVCOOP_CHAT_NO_RETAIN",
    "Developer: never keep a chat line in history (the chat drill's history red).")
CFG_FLAG(chat_seed_suppress, "chat_seed_suppress", "dev", false, "VOTVCOOP_CHAT_SEED_SUPPRESS",
    "Developer: send a joiner no chat history (the seed red).")
