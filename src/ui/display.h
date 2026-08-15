#ifndef LNURLVAULT_DISPLAY_H
#define LNURLVAULT_DISPLAY_H

typedef enum {
    DISPLAY_STATE_IDLE,
    DISPLAY_STATE_CONFIRM_PENDING,
    DISPLAY_STATE_APPROVED,
    DISPLAY_STATE_DECLINED,
} display_state_t;

void display_init(void);
void display_set_state(display_state_t state);

#endif
