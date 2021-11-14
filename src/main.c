#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

#include <SDL2/SDL_scancode.h>

#include "FreeRTOS.h"
#include "queue.h"
#include "semphr.h"
#include "task.h"

#include "TUM_Ball.h"
#include "TUM_Draw.h"
#include "TUM_Font.h"
#include "TUM_Event.h"
#include "TUM_Sound.h"
#include "TUM_Utils.h"
#include "TUM_FreeRTOS_Utils.h"
#include "TUM_Print.h"

#include "AsyncIO.h"

#define mainGENERIC_PRIORITY (tskIDLE_PRIORITY)
#define mainGENERIC_STACK_SIZE ((unsigned short)2560)
#define STACK_SIZE 2000

#define STATE_QUEUE_LENGTH 1

#define STATE_COUNT 2

#define STATE_ONE 0
#define STATE_TWO 1
#define STATE_THREE 2

#define NEXT_TASK 0
#define PREV_TASK 1

#define STARTING_STATE STATE_ONE

#define STATE_DEBOUNCE_DELAY 300

#define KEYCODE(CHAR) SDL_SCANCODE_##CHAR
#define CAVE_SIZE_X SCREEN_WIDTH / 2
#define CAVE_SIZE_Y SCREEN_HEIGHT / 2
#define CAVE_X CAVE_SIZE_X / 2
#define CAVE_Y CAVE_SIZE_Y / 2
#define CAVE_THICKNESS 25
#define RADIUS 50
#define PI 3.14159265
#define FREQ 1
#define LOGO_FILENAME "freertos.jpg"
#define UDP_BUFFER_SIZE 2000
#define UDP_TEST_PORT_1 1234
#define UDP_TEST_PORT_2 4321
#define MSG_QUEUE_BUFFER_SIZE 1000
#define MSG_QUEUE_MAX_MSG_COUNT 10
#define TCP_BUFFER_SIZE 2000
#define TCP_TEST_PORT 2222

#ifdef TRACE_FUNCTIONS
#include "tracer.h"
#endif

const unsigned char next_state_signal = NEXT_TASK;
const unsigned char prev_state_signal = PREV_TASK;

static TaskHandle_t StateMachine = NULL;
static TaskHandle_t BufferSwap = NULL;
static TaskHandle_t Task1 = NULL;
static TaskHandle_t Task2 = NULL;
static TaskHandle_t Task3 = NULL;
static TaskHandle_t Task4 = NULL;
static StaticTask_t Task2Buffer;
static StackType_t xStack[ STACK_SIZE ];

static QueueHandle_t StateQueue = NULL;
static SemaphoreHandle_t DrawSignal = NULL;
static SemaphoreHandle_t ScreenLock = NULL;

static image_handle_t logo_image = NULL;

typedef struct buttons_buffer {
    unsigned char buttons[SDL_NUM_SCANCODES];
    SemaphoreHandle_t lock;
} buttons_buffer_t;

static buttons_buffer_t buttons = { 0 };

void checkDraw(unsigned char status, const char *msg)
{
    if (status) {
        if (msg)
            fprints(stderr, "[ERROR] %s, %s\n", msg,
                    tumGetErrorMessage());
        else {
            fprints(stderr, "[ERROR] %s\n", tumGetErrorMessage());
        }
    }
}

/*
 * Changes the state, either forwards of backwards
 */
void changeState(volatile unsigned char *state, unsigned char forwards)
{
    switch (forwards) {
        case NEXT_TASK:
            if (*state == STATE_COUNT - 1) {
                *state = 0;
            }
            else {
                (*state)++;
            }
            break;
        case PREV_TASK:
            if (*state == 0) {
                *state = STATE_COUNT - 1;
            }
            else {
                (*state)--;
            }
            break;
        default:
            break;
    }
}

void xGetButtonInput(void)
{
    if (xSemaphoreTake(buttons.lock, 0) == pdTRUE) {
        xQueueReceive(buttonInputQueue, &buttons.buttons, 0);
        xSemaphoreGive(buttons.lock);
    }
}

static int vCheckStateInput(void)
{
    if (xSemaphoreTake(buttons.lock, 0) == pdTRUE) {
        if (buttons.buttons[KEYCODE(E)]) {
            buttons.buttons[KEYCODE(E)] = 0;
            if (StateQueue) {
                xSemaphoreGive(buttons.lock);
                xQueueSend(StateQueue, &next_state_signal, 0);
                return 0;
            }
            xSemaphoreGive(buttons.lock);
            return -1;
        }
        xSemaphoreGive(buttons.lock);
        //vTaskDelay(20);
    }

    return 0;
}

/*
 * Example basic state machine with sequential states
 */
void basicSequentialStateMachine(void *pvParameters)
{
    unsigned char current_state = STARTING_STATE; // Default state
    unsigned char state_changed =
        1; // Only re-evaluate state if it has changed
    unsigned char input = 0;

    const int state_change_period = STATE_DEBOUNCE_DELAY;

    TickType_t last_change = xTaskGetTickCount();

    while (1) {
        printf("Checking state change\n");
        xGetButtonInput();
        vCheckStateInput();
        if (state_changed) {
            goto initial_state;
        }

        // Handle state machine input
        if (StateQueue)
            if (xQueueReceive(StateQueue, &input, portMAX_DELAY) ==
                pdTRUE)
                if (xTaskGetTickCount() - last_change >
                    state_change_period) {
                    changeState(&current_state, input);
                    state_changed = 1;
                    last_change = xTaskGetTickCount();
                }

initial_state:
        // Handle current state
        if (state_changed) {
            printf("State changed\n");
            switch (current_state) {
                case STATE_ONE:
                    if (Task3) {
                        vTaskSuspend(Task3);
                    }
                    if (Task4) {
                        vTaskSuspend(Task4);
                    }
                    if (Task1) {
                        vTaskResume(Task1);
                    }
                    if (Task2) {
                        vTaskResume(Task2);
                    }
                    break;
                case STATE_TWO:
                    if (Task1) {
                        vTaskSuspend(Task1);
                    }
                    if (Task2) {
                        vTaskSuspend(Task2);
                    }
                    if (Task3) {
                        vTaskResume(Task3);
                    }
                    if (Task4) {
                        vTaskResume(Task4);
                    }
                    break;
                default:
                    break;
            }
            state_changed = 0;
        }
    }
}

#define FPS_AVERAGE_COUNT 50
#define FPS_FONT "IBMPlexSans-Bold.ttf"

void vDrawFPS(void)
{
    static unsigned int periods[FPS_AVERAGE_COUNT] = { 0 };
    static unsigned int periods_total = 0;
    static unsigned int index = 0;
    static unsigned int average_count = 0;
    static TickType_t xLastWakeTime = 0, prevWakeTime = 0;
    static char str[10] = { 0 };
    static int text_width;
    int fps = 0;
    font_handle_t cur_font = tumFontGetCurFontHandle();

    if (average_count < FPS_AVERAGE_COUNT) {
        average_count++;
    }
    else {
        periods_total -= periods[index];
    }

    xLastWakeTime = xTaskGetTickCount();

    if (prevWakeTime != xLastWakeTime) {
        periods[index] =
            configTICK_RATE_HZ / (xLastWakeTime - prevWakeTime);
        prevWakeTime = xLastWakeTime;
    }
    else {
        periods[index] = 0;
    }

    periods_total += periods[index];

    if (index == (FPS_AVERAGE_COUNT - 1)) {
        index = 0;
    }
    else {
        index++;
    }

    fps = periods_total / average_count;

    tumFontSelectFontFromName(FPS_FONT);

    sprintf(str, "FPS: %2d", fps);

    if (!tumGetTextSize((char *)str, &text_width, NULL))
        checkDraw(tumDrawFilledBox(SCREEN_WIDTH - text_width - 10, SCREEN_HEIGHT - DEFAULT_FONT_SIZE * 1.5, text_width, DEFAULT_FONT_SIZE, White), __FUNCTION__);
        checkDraw(tumDrawText(str, SCREEN_WIDTH - text_width - 10,
                              SCREEN_HEIGHT - DEFAULT_FONT_SIZE * 1.5,
                              Skyblue),
                  __FUNCTION__);

    tumFontSelectFontFromHandle(cur_font);
    tumFontPutFontHandle(cur_font);
}

void vSwapBuffers(void *pvParameters)
{
    TickType_t xLastWakeTime;
    xLastWakeTime = xTaskGetTickCount();
    const TickType_t frameratePeriod = 20;

    tumDrawBindThread(); // Setup Rendering handle with correct GL context

    while (1) {
        printf("Attempt to change frame\n");
        if (xSemaphoreTake(ScreenLock, portMAX_DELAY) == pdTRUE) {
            printf("Frame changed\n");
            vDrawFPS();
            tumDrawUpdateScreen();
            tumEventFetchEvents(FETCH_EVENT_BLOCK);
            xSemaphoreGive(ScreenLock);
            //xSemaphoreGive(DrawSignal);
            vTaskDelayUntil(&xLastWakeTime,
                            pdMS_TO_TICKS(frameratePeriod));
        }
    }
}

void vDrawCaveBoundingBox(void)
{
    checkDraw(tumDrawFilledBox(CAVE_X - CAVE_THICKNESS,
                               CAVE_Y - CAVE_THICKNESS,
                               CAVE_SIZE_X + CAVE_THICKNESS * 2,
                               CAVE_SIZE_Y + CAVE_THICKNESS * 2, TUMBlue),
              __FUNCTION__);

    checkDraw(tumDrawFilledBox(CAVE_X, CAVE_Y, CAVE_SIZE_X, CAVE_SIZE_Y,
                               Aqua),
              __FUNCTION__);
}

void vDrawCave(unsigned char ball_color_inverted)
{
    static unsigned short circlePositionX, circlePositionY;

    vDrawCaveBoundingBox();

    circlePositionX = CAVE_X + tumEventGetMouseX() / 2;
    circlePositionY = CAVE_Y + tumEventGetMouseY() / 2;

    if (ball_color_inverted)
        checkDraw(tumDrawCircle(circlePositionX, circlePositionY, 20,
                                Black),
                  __FUNCTION__);
    else
        checkDraw(tumDrawCircle(circlePositionX, circlePositionY, 20,
                                Silver),
                  __FUNCTION__);
}

void vDrawHelpText(void)
{
    static char str[100] = { 0 };
    static int text_width;
    ssize_t prev_font_size = tumFontGetCurFontSize();

    tumFontSetSize((ssize_t)30);

    sprintf(str, "[Q]uit");

    if (!tumGetTextSize((char *)str, &text_width, NULL))
        checkDraw(tumDrawText(str, SCREEN_WIDTH - text_width - 10,
                              DEFAULT_FONT_SIZE * 0.5, Black),
                  __FUNCTION__);

    tumFontSetSize(prev_font_size);
}

void vDrawLogo(void)
{
    static int image_height;

    if ((image_height = tumDrawGetLoadedImageHeight(logo_image)) != -1)
        checkDraw(tumDrawLoadedImage(logo_image, 10,
                                     SCREEN_HEIGHT - 10 - image_height),
                  __FUNCTION__);
    else {
        fprints(stderr,
                "Failed to get size of image '%s', does it exist?\n",
                LOGO_FILENAME);
    }
}

void vDrawStaticItems(void)
{
    vDrawHelpText();
    vDrawLogo();
}

void playBallSound(void *args)
{
    tumSoundPlaySample(a3);
}

int vCheckButtonInput(int key, int *n, int *button_flag)
{
    if (xSemaphoreTake(buttons.lock, 0) == pdTRUE) {
        if (buttons.buttons[key] && *button_flag == 0) {
            buttons.buttons[key] = 0;
            *button_flag = 1;
            (*n)++;
            printf("%d\n", *n);
            xSemaphoreGive(buttons.lock);
            return 1;
        } else if (buttons.buttons[key] && *button_flag == 1) {
            xSemaphoreGive(buttons.lock);
            return 0;
        }
        *button_flag = 0;
        xSemaphoreGive(buttons.lock);
        return 0;
    }
    return 0;
}

void vTask1(void *pvParameters)
{
    TickType_t xLastWakeTime;
    printf("Task 1 init'd\n");

    ball_t *my_circle = createBall(SCREEN_WIDTH / 4, SCREEN_HEIGHT / 2, Red,
                                 RADIUS, 1000, &playBallSound, NULL);

    ball_t *my_circle2 = createBall(SCREEN_WIDTH / 4, SCREEN_HEIGHT / 2, White,
                                 RADIUS, 1000, &playBallSound, NULL);

    xLastWakeTime = xTaskGetTickCount();

    while (1) {
        //if (DrawSignal)
            //if (xSemaphoreTake(DrawSignal, portMAX_DELAY) == pdTRUE) {
                printf("hello 1\n");
                xGetButtonInput(); // Update global input

                xSemaphoreTake(ScreenLock, portMAX_DELAY);
                printf("Draw circle 1\n");

                // Clear screen
                //checkDraw(tumDrawClear(White), __FUNCTION__);
                vDrawStaticItems();
                
                checkDraw(tumDrawCircle(my_circle->x, my_circle->y, my_circle->radius, my_circle->colour), __FUNCTION__);
                xSemaphoreGive(ScreenLock);
                vTaskDelayUntil(&xLastWakeTime, 500);
                printf("Back to work 1\n");
                xSemaphoreTake(ScreenLock, portMAX_DELAY);
                printf("Delete circle 1\n");
                checkDraw(tumDrawCircle(my_circle2->x, my_circle2->y, my_circle2->radius, my_circle2->colour), __FUNCTION__);
                xSemaphoreGive(ScreenLock);
                vTaskDelayUntil(&xLastWakeTime, 500);
                
                // Draw FPS in lower right corner
                

                //xSemaphoreGive(ScreenLock);
                //vCheckStateInput();
                printf("bye 1\n");
            //}
    }
}

void vTask2(void *pvParameters)
{
    TickType_t xLastWakeTime;
    printf("Task 2 init'd\n");

    ball_t *my_circle = createBall(SCREEN_WIDTH * 3/4, SCREEN_HEIGHT / 2, Green,
                                 RADIUS, 1000, &playBallSound, NULL);

    ball_t *my_circle2 = createBall(SCREEN_WIDTH * 3/4, SCREEN_HEIGHT / 2, White,
                                 RADIUS, 1000, &playBallSound, NULL);

    xLastWakeTime = xTaskGetTickCount();

    while (1) {
        //if (DrawSignal)
            //if (xSemaphoreTake(DrawSignal, portMAX_DELAY) == pdTRUE) {
                printf("hello 2\n");

                xGetButtonInput(); // Update global button data

                xSemaphoreTake(ScreenLock, portMAX_DELAY);
                printf("Draw circle 2\n");
                //checkDraw(tumDrawClear(White), __FUNCTION__);

                vDrawStaticItems();

                checkDraw(tumDrawCircle(my_circle->x, my_circle->y, my_circle->radius, my_circle->colour), __FUNCTION__);
                xSemaphoreGive(ScreenLock);
                vTaskDelayUntil(&xLastWakeTime, 250);
                printf("Back to work 2\n");
                xSemaphoreTake(ScreenLock, portMAX_DELAY);
                printf("Delete circle 2\n");
                checkDraw(tumDrawCircle(my_circle2->x, my_circle2->y, my_circle2->radius, my_circle2->colour), __FUNCTION__);
                xSemaphoreGive(ScreenLock);
                vTaskDelayUntil(&xLastWakeTime, 250);
  
                // Draw FPS in lower right corner
                

                //vCheckStateInput();
                printf("bye 2\n");
            //}
    }
}

void vTask3(void *pvParameters)
{
    prints("Task 3 init'd\n");

    while (1) {
        if (DrawSignal)
            if (xSemaphoreTake(DrawSignal, portMAX_DELAY) ==
                pdTRUE) {
                printf("hello 3\n");
                xGetButtonInput(); // Update global button data

                if (xSemaphoreTake(buttons.lock, portMAX_DELAY) == pdTRUE) {
                    if (buttons.buttons[KEYCODE(L)]) {
                        xSemaphoreTake(ScreenLock, portMAX_DELAY);
                        // Clear screen
                        checkDraw(tumDrawClear(White), __FUNCTION__);

                        checkDraw(tumDrawCircle(200, 200, 30, Blue), __FUNCTION__);

                        vDrawStaticItems();

                        // Draw FPS in lower right corner
                        vDrawFPS();

                        xSemaphoreGive(ScreenLock);
                    }
                    xSemaphoreGive(buttons.lock);
                }
                
                //vCheckStateInput();
            }
    }
}

void vTask4(void *pvParameters)
{
    prints("Task 4 init'd\n");

    while (1) {
        if (DrawSignal)
            if (xSemaphoreTake(DrawSignal, portMAX_DELAY) ==
                pdTRUE) {
                printf("hello 4\n");
                xGetButtonInput(); // Update global button data

                if (xSemaphoreTake(buttons.lock, portMAX_DELAY) == pdTRUE) {
                    if (buttons.buttons[KEYCODE(R)]) {
                        xSemaphoreTake(ScreenLock, portMAX_DELAY);
                        // Clear screen
                        checkDraw(tumDrawClear(White), __FUNCTION__);

                        checkDraw(tumDrawCircle(200, 200, 30, Black), __FUNCTION__);

                        vDrawStaticItems();

                        // Draw FPS in lower right corner
                        vDrawFPS();

                        xSemaphoreGive(ScreenLock);
                    }
                    xSemaphoreGive(buttons.lock);
                }

                //vCheckStateInput();
            }
    }
}

#define PRINT_TASK_ERROR(task) PRINT_ERROR("Failed to print task ##task");

int main(int argc, char *argv[])
{
    char *bin_folder_path = tumUtilGetBinFolderPath(argv[0]);

    prints("Initializing: ");

    //  Note PRINT_ERROR is not thread safe and is only used before the
    //  scheduler is started. There are thread safe print functions in
    //  TUM_Print.h, `prints` and `fprints` that work exactly the same as
    //  `printf` and `fprintf`. So you can read the documentation on these
    //  functions to understand the functionality.

    if (tumDrawInit(bin_folder_path)) {
        PRINT_ERROR("Failed to intialize drawing");
        goto err_init_drawing;
    }
    else {
        prints("drawing");
    }

    if (tumEventInit()) {
        PRINT_ERROR("Failed to initialize events");
        goto err_init_events;
    }
    else {
        prints(", events");
    }

    if (tumSoundInit(bin_folder_path)) {
        PRINT_ERROR("Failed to initialize audio");
        goto err_init_audio;
    }
    else {
        prints(", and audio\n");
    }

    if (safePrintInit()) {
        PRINT_ERROR("Failed to init safe print");
        goto err_init_safe_print;
    }

    logo_image = tumDrawLoadImage(LOGO_FILENAME);

    atexit(aIODeinit);

    //Load a second font for fun
    tumFontLoadFont(FPS_FONT, DEFAULT_FONT_SIZE);

    buttons.lock = xSemaphoreCreateMutex(); // Locking mechanism
    if (!buttons.lock) {
        PRINT_ERROR("Failed to create buttons lock");
        goto err_buttons_lock;
    }

    DrawSignal = xSemaphoreCreateBinary(); // Screen buffer locking
    if (!DrawSignal) {
        PRINT_ERROR("Failed to create draw signal");
        goto err_draw_signal;
    }
    ScreenLock = xSemaphoreCreateMutex();
    if (!ScreenLock) {
        PRINT_ERROR("Failed to create screen lock");
        goto err_screen_lock;
    }

    // Message sending
    StateQueue = xQueueCreate(STATE_QUEUE_LENGTH, sizeof(unsigned char));
    if (!StateQueue) {
        PRINT_ERROR("Could not open state queue");
        goto err_state_queue;
    }

    if (xTaskCreate(basicSequentialStateMachine, "StateMachine",
                    mainGENERIC_STACK_SIZE * 2, NULL,
                    configMAX_PRIORITIES - 1, StateMachine) != pdPASS) {
        PRINT_TASK_ERROR("StateMachine");
        goto err_statemachine;
    }
    if (xTaskCreate(vSwapBuffers, "BufferSwapTask",
                    mainGENERIC_STACK_SIZE * 2, NULL, configMAX_PRIORITIES,
                    BufferSwap) != pdPASS) {
        PRINT_TASK_ERROR("BufferSwapTask");
        goto err_bufferswap;
    }

    /** Demo Tasks */
    if (xTaskCreate(vTask1, "Task1", mainGENERIC_STACK_SIZE * 2,
                    NULL, mainGENERIC_PRIORITY + 5, &Task1) != pdPASS) {
        PRINT_TASK_ERROR("Task1");
        goto err_task1;
    }

    Task2 = xTaskCreateStatic(vTask2, "Task2", STACK_SIZE,
                    NULL, mainGENERIC_PRIORITY + 4, xStack, &Task2Buffer);
    if (Task2 == NULL) {
        PRINT_TASK_ERROR("Task2");
        goto err_task2;
    }

    if (xTaskCreate(vTask3, "Task3", mainGENERIC_STACK_SIZE * 2,
                    NULL, mainGENERIC_PRIORITY + 3, &Task3) != pdPASS) {
        PRINT_TASK_ERROR("Task3");
        goto err_task3;
    }

    if (xTaskCreate(vTask4, "Task4", mainGENERIC_STACK_SIZE * 2,
                    NULL, mainGENERIC_PRIORITY + 2, &Task4) != pdPASS) {
        PRINT_TASK_ERROR("Task4");
        goto err_task4;
    }
    
    //tumFUtilPrintTaskStateList();

    vTaskStartScheduler();

    return EXIT_SUCCESS;

    err_task4:
        vTaskDelete(Task3);
    err_task3:
        vTaskDelete(Task2);
    err_task2:
        vTaskDelete(Task1);
    err_task1:
        vTaskDelete(BufferSwap);
    err_bufferswap:
        vTaskDelete(StateMachine);
    err_statemachine:
        vQueueDelete(StateQueue);
    err_state_queue:
        vSemaphoreDelete(ScreenLock);
    err_screen_lock:
        vSemaphoreDelete(DrawSignal);
    err_draw_signal:
        vSemaphoreDelete(buttons.lock);
    err_buttons_lock:
        tumSoundExit();
    err_init_audio:
        tumEventExit();
    err_init_events:
        tumDrawExit();
    err_init_drawing:
        safePrintExit();
    err_init_safe_print:
    return EXIT_FAILURE;
}

// cppcheck-suppress unusedFunction
__attribute__((unused)) void vMainQueueSendPassed(void)
{
    /* This is just an example implementation of the "queue send" trace hook. */
}

// cppcheck-suppress unusedFunction
__attribute__((unused)) void vApplicationIdleHook(void)
{
#ifdef __GCC_POSIX__
    struct timespec xTimeToSleep, xTimeSlept;
    /* Makes the process more agreeable when using the Posix simulator. */
    xTimeToSleep.tv_sec = 1;
    xTimeToSleep.tv_nsec = 0;
    nanosleep(&xTimeToSleep, &xTimeSlept);
#endif
}
