# hyprv — open questions

Pick one before starting work in the affected area. Index: [`../PLAN.md`](../PLAN.md).
1. **VM kernel debugger transport.** KDNET (modern, net-based) vs. serial COM via named pipe (what VMPlex used). KDNET requires guest-side configuration we may not have a clean path to apply from outside the guest; serial via COM is host-side Hyper-V config we can do directly via WMI. Decision shapes the entire debugger implementation. Recommendation: serial-COM-via-named-pipe for the first cut, KDNET later as an opt-in for users who can configure their guests.
2. ~~**Tear-away tabs.**~~ ✅ **resolved + shipped this session** (legacy drag-drop, not the native `CanTearOutTabs` the original note assumed — see the Done block + gotchas #44–48). Open follow-up: per-window session persistence for torn windows (v1 only persists the primary).
3. **Settings schema migrations.** Each new field is additive (`HasKey` guards default missing ones gracefully). Revisit if we ever rename or remove fields.
4. **`SystemAccentColor` theme reactivity.** Tab underline gradient hardcodes the accent color at parse time via `{ThemeResource SystemAccentColor}`. A user accent change at runtime won't re-render. Low priority.
