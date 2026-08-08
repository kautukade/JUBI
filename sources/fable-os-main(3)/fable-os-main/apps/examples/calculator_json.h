/* calculator_json.h — the reference app document, as the kernel sees it.
 *
 * PURPOSE
 *   apps/examples/calculator.json is the artifact: a hand-written, known-good
 *   calculator in the app document format (include/app.h), written by a human in
 *   exactly the format a model has to emit. This header is that same text
 *   compiled into the kernel, because the kernel has no way to read a host file.
 *
 *   It exists for the same reason vm/programs/ac97_bringup.dvm does: to make the
 *   next failure attributable. When a MODEL is asked for a calculator and the
 *   window is wrong, the question "can the runtime run a calculator at all?" is
 *   already answered — this document launches, at boot, in a host test, and under
 *   a real mouse.
 *
 * KEEPING THE TWO COPIES HONEST
 *   APP_CALCULATOR_JSON below must be byte-for-byte the contents of
 *   calculator.json. tests/host/test_app.c reads that file at run time and fails
 *   if the two ever differ, so editing one and forgetting the other is a red test
 *   rather than a mystery. Edit the .json, then regenerate with:
 *
 *       python3 apps/examples/gen_header.py
 *
 * DEPENDENCIES
 *   None: one string literal, so a freestanding kernel TU and a host test can
 *   both take it.
 */
#ifndef APP_EXAMPLE_CALCULATOR_H
#define APP_EXAMPLE_CALCULATOR_H

#define APP_CALCULATOR_JSON \
/* --- BEGIN GENERATED (do not edit by hand) --- */ \
    "{\n" \
    "  \"note\": \"The hand-written reference app for fable-os: a working calculator, written by a human in the same document format the model emits, so that a model failure is attributable to the model and not to the runtime. Fixed point, six decimals, magnitude up to 9e9; division by zero and overflow produce the error value, which text() renders as the word error and which latches until C.\",\n" \
    "  \"title\": \"Calculator\",\n" \
    "  \"width\": 216,\n" \
    "  \"height\": 266,\n" \
    "  \"grid\": { \"rows\": 6, \"cols\": 4, \"gap\": 5, \"pad\": 6 },\n" \
    "\n" \
    "  \"vars\": { \"acc\": 0, \"rhs\": 0, \"res\": 0, \"op\": \"\", \"fresh\": 1, \"err\": 0 },\n" \
    "\n" \
    "  \"widgets\": [\n" \
    "    { \"kind\": \"field\", \"name\": \"display\", \"text\": \"0\", \"align\": \"right\",\n" \
    "      \"readonly\": true, \"row\": 0, \"col\": 0, \"colspan\": 4 },\n" \
    "\n" \
    "    { \"kind\": \"button\", \"text\": \"7\", \"tag\": \"digit\", \"row\": 1, \"col\": 0 },\n" \
    "    { \"kind\": \"button\", \"text\": \"8\", \"tag\": \"digit\", \"row\": 1, \"col\": 1 },\n" \
    "    { \"kind\": \"button\", \"text\": \"9\", \"tag\": \"digit\", \"row\": 1, \"col\": 2 },\n" \
    "    { \"kind\": \"button\", \"text\": \"/\", \"tag\": \"op\",    \"row\": 1, \"col\": 3 },\n" \
    "\n" \
    "    { \"kind\": \"button\", \"text\": \"4\", \"tag\": \"digit\", \"row\": 2, \"col\": 0 },\n" \
    "    { \"kind\": \"button\", \"text\": \"5\", \"tag\": \"digit\", \"row\": 2, \"col\": 1 },\n" \
    "    { \"kind\": \"button\", \"text\": \"6\", \"tag\": \"digit\", \"row\": 2, \"col\": 2 },\n" \
    "    { \"kind\": \"button\", \"text\": \"*\", \"tag\": \"op\",    \"row\": 2, \"col\": 3 },\n" \
    "\n" \
    "    { \"kind\": \"button\", \"text\": \"1\", \"tag\": \"digit\", \"row\": 3, \"col\": 0 },\n" \
    "    { \"kind\": \"button\", \"text\": \"2\", \"tag\": \"digit\", \"row\": 3, \"col\": 1 },\n" \
    "    { \"kind\": \"button\", \"text\": \"3\", \"tag\": \"digit\", \"row\": 3, \"col\": 2 },\n" \
    "    { \"kind\": \"button\", \"text\": \"-\", \"tag\": \"op\",    \"row\": 3, \"col\": 3 },\n" \
    "\n" \
    "    { \"kind\": \"button\", \"text\": \"0\", \"tag\": \"digit\", \"row\": 4, \"col\": 0 },\n" \
    "    { \"kind\": \"button\", \"text\": \".\", \"name\": \"dot\",  \"row\": 4, \"col\": 1 },\n" \
    "    { \"kind\": \"button\", \"text\": \"C\", \"name\": \"clear\", \"row\": 4, \"col\": 2 },\n" \
    "    { \"kind\": \"button\", \"text\": \"+\", \"tag\": \"op\",    \"row\": 4, \"col\": 3 },\n" \
    "\n" \
    "    { \"kind\": \"button\", \"text\": \"=\", \"name\": \"equals\", \"row\": 5, \"col\": 0,\n" \
    "      \"colspan\": 4 }\n" \
    "  ],\n" \
    "\n" \
    "  \"on\": [\n" \
    "    { \"click\": \"clear\", \"do\": [\n" \
    "      { \"set\": \"acc\", \"to\": \"0\" },\n" \
    "      { \"set\": \"rhs\", \"to\": \"0\" },\n" \
    "      { \"set\": \"res\", \"to\": \"0\" },\n" \
    "      { \"set\": \"op\", \"to\": \"''\" },\n" \
    "      { \"set\": \"fresh\", \"to\": \"1\" },\n" \
    "      { \"set\": \"err\", \"to\": \"0\" },\n" \
    "      { \"set\": \"display\", \"to\": \"'0'\" }\n" \
    "    ] },\n" \
    "\n" \
    "    { \"click\": \"digit\", \"do\": [\n" \
    "      { \"if\": \"err\", \"then\": [ { \"stop\": true } ] },\n" \
    "      { \"if\": \"fresh\", \"then\": [\n" \
    "        { \"set\": \"display\", \"to\": \"''\" },\n" \
    "        { \"set\": \"fresh\", \"to\": \"0\" }\n" \
    "      ] },\n" \
    "      { \"if\": \"digits(display) >= 12\", \"then\": [ { \"stop\": true } ] },\n" \
    "      { \"if\": \"display == '0'\", \"then\": [ { \"set\": \"display\", \"to\": \"''\" } ] },\n" \
    "      { \"set\": \"display\", \"to\": \"cat(display, key)\" }\n" \
    "    ] },\n" \
    "\n" \
    "    { \"click\": \"dot\", \"do\": [\n" \
    "      { \"if\": \"err\", \"then\": [ { \"stop\": true } ] },\n" \
    "      { \"if\": \"fresh\", \"then\": [\n" \
    "        { \"set\": \"display\", \"to\": \"'0'\" },\n" \
    "        { \"set\": \"fresh\", \"to\": \"0\" }\n" \
    "      ] },\n" \
    "      { \"if\": \"has(display, '.')\", \"then\": [ { \"stop\": true } ] },\n" \
    "      { \"if\": \"display == ''\", \"then\": [ { \"set\": \"display\", \"to\": \"'0'\" } ] },\n" \
    "      { \"set\": \"display\", \"to\": \"cat(display, '.')\" }\n" \
    "    ] },\n" \
    "\n" \
    "    { \"click\": [ \"op\", \"equals\" ], \"do\": [\n" \
    "      { \"if\": \"err\", \"then\": [ { \"stop\": true } ] },\n" \
    "      { \"set\": \"rhs\", \"to\": \"num(display)\" },\n" \
    "      { \"if\": \"op == ''\",  \"then\": [ { \"set\": \"res\", \"to\": \"rhs\" } ] },\n" \
    "      { \"if\": \"op == '+'\", \"then\": [ { \"set\": \"res\", \"to\": \"acc + rhs\" } ] },\n" \
    "      { \"if\": \"op == '-'\", \"then\": [ { \"set\": \"res\", \"to\": \"acc - rhs\" } ] },\n" \
    "      { \"if\": \"op == '*'\", \"then\": [ { \"set\": \"res\", \"to\": \"acc * rhs\" } ] },\n" \
    "      { \"if\": \"op == '/'\", \"then\": [ { \"set\": \"res\", \"to\": \"acc / rhs\" } ] },\n" \
    "      { \"set\": \"acc\", \"to\": \"res\" },\n" \
    "      { \"set\": \"display\", \"to\": \"text(res)\" },\n" \
    "      { \"if\": \"iserr(res)\", \"then\": [ { \"set\": \"err\", \"to\": \"1\" } ] },\n" \
    "      { \"if\": \"key == '='\", \"then\": [ { \"set\": \"op\", \"to\": \"''\" } ],\n" \
    "                            \"else\": [ { \"set\": \"op\", \"to\": \"key\" } ] },\n" \
    "      { \"set\": \"fresh\", \"to\": \"1\" }\n" \
    "    ] }\n" \
    "  ]\n" \
    "}\n"
/* --- END GENERATED --- */

#endif /* APP_EXAMPLE_CALCULATOR_H */
