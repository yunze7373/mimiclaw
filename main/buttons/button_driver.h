#ifndef BUTTON_BSP_H
#define BUTTON_BSP_H
#include <stdio.h>
#include <stdbool.h>  
#include "buttons/multi_button.h"
#include "mimi_config.h"


#define BOOT_KEY_PIN   MIMI_PIN_BOOT_KEY
#define VOL_DOWN_PIN   MIMI_PIN_VOL_DOWN
#define VOL_UP_PIN     MIMI_PIN_VOL_UP

#define Button_PIN1   BOOT_KEY_PIN
#define Button_PIN2   VOL_DOWN_PIN
#define Button_PIN3   VOL_UP_PIN

extern PressEvent BOOT_KEY_State;    
extern PressEvent VOL_DOWN_State;
extern PressEvent VOL_UP_State;

void button_Init(void);

#endif
