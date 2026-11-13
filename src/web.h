#pragma once
/* Browser-based file manager: http://<ps4-ip>:8080 */

#define WEB_PORT 8080

int  web_start(void);
void web_stop(void);
int  web_running(void);
int  web_requests(void);
/* Last few actions, newest first. Returns 0 if no line i. */
int  web_log_line(int i, char *out, int n);
