#pragma once

// Gmail over IMAP. See TDeckMail.cpp - step one is connect/authenticate/count only, built first
// to prove certificate verification and the heap before an inbox UI is stacked on top.
//
// Credentials live on the SD card in /gmail.txt (address, then the 16-character app password),
// never compiled into the firmware, so the installer stays shareable.

#ifdef __cplusplus
extern "C" {
#endif

// Ask for an inbox check. false if one is already running. Safe from the UI task: it only
// records the intent; tdeck_mail_service() on the main loop does the work.
bool tdeck_mail_check(void);

// Connect, verify the certificate, read the IMAP greeting, log out. NO credentials involved,
// so it proves the certificate and the heap on their own - and later tells "network/certificate
// is wrong" apart from "login is wrong", which otherwise look identical from the device.
bool tdeck_mail_connect_test(void);

// Drop the cached address/password so /gmail.txt is read again. Called by the setup form after
// it writes the file.
void tdeck_mail_forget_creds(void);

// 0 = still working, 1 = done, -1 = failed (see tdeck_mail_error()).
int tdeck_mail_poll(void);

// Last successful counts; -1 if unknown.
void tdeck_mail_counts(int *total, int *unseen);

// Human-readable reason the last attempt failed. Never contains the password.
const char *tdeck_mail_error(void);

// ---- reading ---------------------------------------------------------------------------
// Fetch one PAGE of inbox headers, newest first. page 0 is the newest. ⭐ Only ever one page is
// requested from the server or held in memory - Jake's inbox has ~7,000 messages and the cost
// here is the same as for twelve.
bool tdeck_mail_list(int page);
int tdeck_mail_list_count(void);  // headers actually fetched for this page
int tdeck_mail_total(void);       // messages in the inbox
int tdeck_mail_page(void);        // which page the last list was
bool tdeck_mail_item(int i, unsigned *seq, const char **from, const char **subj, const char **date, bool *seen);

// Fetch one message and turn it into plain text. Uses BODY.PEEK, so opening a message does NOT
// mark it read - tapping through an inbox should not quietly clear 7,000 unread flags.
bool tdeck_mail_read(unsigned seq);
const char *tdeck_mail_body(void);

// ---- sending ---------------------------------------------------------------------------
bool tdeck_mail_send(const char *to, const char *subject, const char *body);
// asReply: thread it under the message last read (In-Reply-To / References), as Reply does.
bool tdeck_mail_send_ex(const char *to, const char *subject, const char *body, bool asReply);

// The message last read, for Reply and Forward. The address is Reply-To if set, else From.
const char *tdeck_mail_read_reply_addr(void);
const char *tdeck_mail_read_from(void);
const char *tdeck_mail_read_date(void);
const char *tdeck_mail_read_subject(void);

// Send a fixed test message to the signed-in account itself. Takes no recipient on
// purpose - see the note in the .cpp.
bool tdeck_mail_send_selftest(void);

// Called from loop() in main.cpp, NOT from the UI task.
void tdeck_mail_service(void);

#ifdef __cplusplus
}
#endif
