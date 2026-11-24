#ifndef XENUS_AUTH_SCREEN_H
#define XENUS_AUTH_SCREEN_H

int auth_screen_open(void);
void auth_screen_close(void);
int auth_screen_active(void);
int auth_screen_handle_event(XEvent *ev);
void auth_screen_draw(void);

#endif
