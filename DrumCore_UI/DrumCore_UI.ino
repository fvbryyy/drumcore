#include "config.h"
#include "display.h"
#include "encoder.h"
#include "menu.h"
#include "screens.h"
#include "ui.h"
#include "uart_client.h"

void setup()
{
    uiInit();
}

void loop()
{
    uiUpdate();
}
