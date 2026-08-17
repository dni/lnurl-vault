#ifndef LNURLVAULT_SERIAL_UART_H
#define LNURLVAULT_SERIAL_UART_H

/* Starts the UART host transport and its service task. Boards reach this
 * through board_serial_start(); nothing else should call it directly. */
void serial_uart_start(void);

#endif
