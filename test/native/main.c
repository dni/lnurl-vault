#include <stdio.h>

#include "unity_lite.h"

int g_tests_run = 0;
int g_tests_failed = 0;

void test_sha256_run(void);
void test_hex_run(void);
void test_json_run(void);
void test_vault_run(void);
void test_dispatcher_run(void);
void test_secret_leak_run(void);
void test_button_fsm_run(void);
void test_note_url_run(void);
void test_ble_frame_run(void);
void test_note_display_run(void);
void test_line_proto_run(void);
void test_approval_run(void);
void test_regressions_run(void);
void test_gated_actions_run(void);
void test_wipe_run(void);
void test_list_paging_run(void);
void test_qr_capacity_run(void);
void test_base64_run(void);
void test_ota_sign_run(void);
void test_ota_dispatch_run(void);

int main(void) {
    test_sha256_run();
    test_hex_run();
    test_json_run();
    test_vault_run();
    test_dispatcher_run();
    test_secret_leak_run();
    test_button_fsm_run();
    test_note_url_run();
    test_ble_frame_run();
    test_note_display_run();
    test_line_proto_run();
    test_approval_run();
    test_regressions_run();
    test_gated_actions_run();
    test_wipe_run();
    test_list_paging_run();
    test_qr_capacity_run();
    test_base64_run();
    test_ota_sign_run();
    test_ota_dispatch_run();

    printf("%d/%d assertions passed\n", g_tests_run - g_tests_failed, g_tests_run);
    if (g_tests_failed > 0) {
        printf("FAILED: %d assertion(s) failed\n", g_tests_failed);
        return 1;
    }
    printf("All tests passed.\n");
    return 0;
}
