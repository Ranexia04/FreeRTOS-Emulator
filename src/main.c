#include <math.h>
#include <stdio.h>
#include <stdlib.h>

#include <SDL2/SDL_scancode.h>

#include "FreeRTOS.h"
#include "queue.h"
#include "semphr.h"
#include "task.h"
#include "timers.h"

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
#define STACK_SIZE mainGENERIC_STACK_SIZE * 2
#define NUM_TIMERS 1

#define STATE_QUEUE_LENGTH 1

#define STATE_COUNT 3

#define STATE_ONE 0
#define STATE_TWO 1
#define STATE_THREE 2

#define NEXT_TASK 0
#define PREV_TASK 1

#define STARTING_STATE STATE_ONE

#define STATE_DEBOUNCE_DELAY 300

#define KEYCODE(CHAR) SDL_SCANCODE_##CHAR
#define RADIUS 50
#define LOGO_FILENAME "freertos.jpg"
#define FPS_AVERAGE_COUNT 50
#define FPS_FONT "IBMPlexSans-Bold.ttf"

#ifdef TRACE_FUNCTIONS
#include "tracer.h"
#endif

const unsigned char next_state_signal = NEXT_TASK;
const unsigned char prev_state_signal = PREV_TASK;

static TaskHandle_t StateMachine = NULL;
static TaskHandle_t BufferSwap = NULL;
static TaskHandle_t SolutionSwaper = NULL;
static TaskHandle_t Reseter = NULL;
static TaskHandle_t Task1 = NULL;
static TaskHandle_t Task2 = NULL;
static TaskHandle_t Task3 = NULL;
static TaskHandle_t Task4 = NULL;
static TaskHandle_t Task5 = NULL;

static StaticTask_t Task2Buffer;
static StackType_t xStack[ STACK_SIZE ];

static QueueHandle_t StateQueue = NULL;
static QueueHandle_t CurrentStateQueue = NULL;
static QueueHandle_t Task5State = NULL;
static SemaphoreHandle_t DrawSignal = NULL;
static SemaphoreHandle_t ScreenLock = NULL;
static SemaphoreHandle_t SyncSignal = NULL;

static TimerHandle_t xTimers;

static image_handle_t logo_image = NULL;

typedef struct buttons_buffer {
    unsigned char buttons[SDL_NUM_SCANCODES];
    SemaphoreHandle_t lock;
} buttons_buffer_t;

static buttons_buffer_t buttons = { 0 };

typedef struct solution3_buffer {
    int n3;
    int n4;
    int n5;
    SemaphoreHandle_t lock;
} solution3_buffer_t;

static solution3_buffer_t solution3 = { 0 };

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
    }

    return 0;
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

/*
 * Example basic state machine with sequential states
 */
void basicSequentialStateMachine(void *pvParameters)
{
    unsigned char current_state = STARTING_STATE; // Default state
    unsigned char state_changed =
        1; // Only re-evaluate state if it has changed
    unsigned char input = 0, task5_state = 1;
    xQueueOverwrite(Task5State, &task5_state);

    const int state_change_period = STATE_DEBOUNCE_DELAY;

    TickType_t last_change = xTaskGetTickCount();

    char str[5];
    int text_width;

    while (1) {
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
                    xQueueOverwrite(CurrentStateQueue, &current_state);
                    state_changed = 1;
                    last_change = xTaskGetTickCount();
                }

initial_state:
        // Handle current state
        if (state_changed) {
            switch (current_state) {
                case STATE_ONE:
                    if (Reseter) {
                        vTaskSuspend(Reseter);
                    }
                    if (Task3) {
                        vTaskSuspend(Task3);
                    }
                    if (Task4) {
                        vTaskSuspend(Task4);
                    }
                    if (Task5) {
                        vTaskSuspend(Task5);
                    }
                    xSemaphoreTake(DrawSignal, portMAX_DELAY);
                    xSemaphoreTake(ScreenLock, portMAX_DELAY);
                    checkDraw(tumDrawClear(White), __FUNCTION__);
                    vDrawStaticItems();
                    xSemaphoreGive(ScreenLock);
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
                    if (Task5) {
                        vTaskSuspend(Task5);
                    }
                    xSemaphoreTake(DrawSignal, portMAX_DELAY);
                    xSemaphoreTake(ScreenLock, portMAX_DELAY);
                    checkDraw(tumDrawClear(White), __FUNCTION__);
                    vDrawStaticItems();
                    sprintf(str, "%d", solution3.n3);
                    tumGetTextSize((char *)str, &text_width, NULL);
                    checkDraw(tumDrawText(str, SCREEN_WIDTH / 4 - text_width / 2,
                            SCREEN_HEIGHT / 2 - DEFAULT_FONT_SIZE / 2, Black),
                            __FUNCTION__);
                    sprintf(str, "%d", solution3.n4);
                    tumGetTextSize((char *)str, &text_width, NULL);
                    checkDraw(tumDrawText(str, SCREEN_WIDTH *3/4 - text_width / 2,
                                SCREEN_HEIGHT / 2 - DEFAULT_FONT_SIZE / 2, Black),
                                __FUNCTION__);
                    xSemaphoreGive(ScreenLock);
                    if (Reseter) {
                        vTaskResume(Reseter);
                    }
                    if (Task3) {
                        vTaskResume(Task3);
                    }
                    if (Task4) {
                        vTaskResume(Task4);
                    }
                    break;
                case STATE_THREE:
                    if (Reseter) {
                        vTaskSuspend(Reseter);
                    }
                    if (Task1) {
                        vTaskSuspend(Task1);
                    }
                    if (Task2) {
                        vTaskSuspend(Task2);
                    }
                    if (Task3) {
                        vTaskSuspend(Task4);
                    }
                    if (Task4) {
                        vTaskSuspend(Task4);
                    }
                    xSemaphoreTake(DrawSignal, portMAX_DELAY);
                    xSemaphoreTake(ScreenLock, portMAX_DELAY);
                    checkDraw(tumDrawClear(White), __FUNCTION__);
                    vDrawStaticItems();
                    sprintf(str, "%d", solution3.n5);
                    tumGetTextSize((char *)str, &text_width, NULL);
                    checkDraw(tumDrawText(str, SCREEN_WIDTH / 2 - text_width / 2,
                            SCREEN_HEIGHT / 2 - DEFAULT_FONT_SIZE / 2, Black),
                            __FUNCTION__);
                    xSemaphoreGive(ScreenLock);
                    xQueuePeek(Task5State, &task5_state, portMAX_DELAY);
                    if (Task5 && task5_state == 1) {
                        vTaskResume(Task5);
                    }
                    break;

                default:
                    break;
            }
            state_changed = 0;
        }
    }
}

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
        checkDraw(tumDrawFilledBox(SCREEN_WIDTH - text_width - 10, SCREEN_HEIGHT - DEFAULT_FONT_SIZE * 1.5,
            text_width, DEFAULT_FONT_SIZE, White), __FUNCTION__);
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
        xSemaphoreTake(ScreenLock, portMAX_DELAY);
        tumDrawUpdateScreen();
        tumEventFetchEvents(FETCH_EVENT_BLOCK);
        xSemaphoreGive(DrawSignal);
        xSemaphoreGive(ScreenLock);
        vTaskDelayUntil(&xLastWakeTime,
            pdMS_TO_TICKS(frameratePeriod));
    }
}

int vCheckButtonInput(int key)
{
    unsigned char current_state, task5_state;

    if (xSemaphoreTake(buttons.lock, 0) == pdTRUE) {
        if (buttons.buttons[key]) {
            buttons.buttons[key] = 0;
            xSemaphoreGive(buttons.lock);
            if (key == KEYCODE(3))
                if (xQueuePeek(CurrentStateQueue, &current_state, 0) == pdTRUE) {
                    if (current_state == STATE_TWO)
                        xSemaphoreGive(SyncSignal);
                }
                
            if (key == KEYCODE(4))
                if (xQueuePeek(CurrentStateQueue, &current_state, 0) == pdTRUE) {
                    if (current_state == STATE_TWO)
                        xTaskNotifyGive(Task4);
                }

            if (key == KEYCODE(5)) {
                if (xQueuePeek(CurrentStateQueue, &current_state, 0) == pdTRUE) {
                    if (eTaskGetState(Task5) == eSuspended && current_state == 2) {
                        task5_state = 1;
                        xQueueOverwrite(Task5State, &task5_state);
                        vTaskResume(Task5);
                    } else if ((eTaskGetState(Task5) == eRunning || eTaskGetState(Task5) == eReady || 
                                eTaskGetState(Task5) == eBlocked) && current_state == 2) {
                        task5_state = 0;
                        xQueueOverwrite(Task5State, &task5_state);
                        vTaskSuspend(Task5);
                    }
                }
            }
            return 0;
        }
        xSemaphoreGive(buttons.lock);
    }
    return 0;
}

void vSolutionSwaper(void *pvParameters)
{
    while (1) {
        /*tumEventFetchEvents(FETCH_EVENT_BLOCK |
                                    FETCH_EVENT_NO_GL_CHECK);*/
        xGetButtonInput();
        vCheckStateInput();
        vCheckButtonInput(KEYCODE(3));
        vCheckButtonInput(KEYCODE(4));
        vCheckButtonInput(KEYCODE(5));
        vTaskDelay(pdMS_TO_TICKS(1));
        //vDrawFPS();
    }
}

void vTimerCallback(TimerHandle_t xTimers)
{
    xSemaphoreTake(solution3.lock, portMAX_DELAY);
    solution3.n3 = 0;
    solution3.n4 = 0;
    xSemaphoreGive(solution3.lock);
    vTaskResume(Reseter);
}

void vReseter(void *pvParameters)
{
    while (1) {
        xTimerStart(xTimers, portMAX_DELAY);
        vTaskSuspend(Reseter);
    }
}

void vTask1(void *pvParameters)
{
    TickType_t xLastWakeTime;
    prints("Task 1 init'd\n");

    ball_t *my_circle = createBall(SCREEN_WIDTH / 4, SCREEN_HEIGHT / 2, Red,
                                 RADIUS, 1000, NULL, NULL);

    xLastWakeTime = xTaskGetTickCount();

    while (1) {
        xSemaphoreTake(DrawSignal, portMAX_DELAY);
        xSemaphoreTake(ScreenLock, portMAX_DELAY);
        my_circle->colour = Red;
        checkDraw(tumDrawCircle(my_circle->x, my_circle->y, my_circle->radius, my_circle->colour), __FUNCTION__);
        xSemaphoreGive(ScreenLock);
            
        vTaskDelayUntil(&xLastWakeTime, pdMS_TO_TICKS(500));
        if (xTaskGetTickCount() - xLastWakeTime - pdMS_TO_TICKS(500) > 1000)
            xLastWakeTime = xTaskGetTickCount();

        xSemaphoreTake(DrawSignal, portMAX_DELAY);        
        xSemaphoreTake(ScreenLock, portMAX_DELAY);
        my_circle->colour = White;
        checkDraw(tumDrawCircle(my_circle->x, my_circle->y, my_circle->radius, my_circle->colour), __FUNCTION__);
        xSemaphoreGive(ScreenLock);

        vTaskDelayUntil(&xLastWakeTime, pdMS_TO_TICKS(500));
        if (xTaskGetTickCount() - xLastWakeTime - pdMS_TO_TICKS(500) > 1000)
            xLastWakeTime = xTaskGetTickCount();
    }
}

void vTask2(void *pvParameters)
{
    TickType_t xLastWakeTime;
    prints("Task 2 init'd\n");

    ball_t *my_circle = createBall(SCREEN_WIDTH * 3/4, SCREEN_HEIGHT / 2, Green,
                                 RADIUS, 1000, NULL, NULL);

    xLastWakeTime = xTaskGetTickCount();

    while (1) {

        xSemaphoreTake(DrawSignal, portMAX_DELAY);
        xSemaphoreTake(ScreenLock, portMAX_DELAY);
        my_circle->colour = Green;
        checkDraw(tumDrawCircle(my_circle->x, my_circle->y, my_circle->radius, my_circle->colour), __FUNCTION__);
        xSemaphoreGive(ScreenLock);

        vTaskDelayUntil(&xLastWakeTime, pdMS_TO_TICKS(250));
        if (xTaskGetTickCount() - xLastWakeTime - pdMS_TO_TICKS(250) > 1000)
            xLastWakeTime = xTaskGetTickCount();

        xSemaphoreTake(DrawSignal, portMAX_DELAY);
        xSemaphoreTake(ScreenLock, portMAX_DELAY);
        my_circle->colour = White;
        checkDraw(tumDrawCircle(my_circle->x, my_circle->y, my_circle->radius, my_circle->colour), __FUNCTION__);
        xSemaphoreGive(ScreenLock);

        vTaskDelayUntil(&xLastWakeTime, pdMS_TO_TICKS(250));
        if (xTaskGetTickCount() - xLastWakeTime - pdMS_TO_TICKS(250) > 1000)
            xLastWakeTime = xTaskGetTickCount();
    }
}

void vTask3(void *pvParameters)
{
    TickType_t last_change;
    last_change = xTaskGetTickCount();

    static char str[5] = { 0 };
    static int text_width;

    sprintf(str, "%d", solution3.n3);
    text_width = tumGetTextSize((char *)str, &text_width, NULL);

    prints("Task 3 init'd\n");

    while (1) {
        xSemaphoreTake(DrawSignal, portMAX_DELAY);
        xSemaphoreTake(SyncSignal, portMAX_DELAY);
        if (xTaskGetTickCount() - last_change > STATE_DEBOUNCE_DELAY) {
            xSemaphoreTake(solution3.lock, portMAX_DELAY);
            solution3.n3++;
            xSemaphoreGive(solution3.lock);
            sprintf(str, "%d", solution3.n3);
            tumGetTextSize((char *)str, &text_width, NULL);
            last_change = xTaskGetTickCount();
        }

        xSemaphoreTake(ScreenLock, portMAX_DELAY);
        checkDraw(tumDrawFilledBox(SCREEN_WIDTH / 4 - text_width / 2,
                SCREEN_HEIGHT / 2 - DEFAULT_FONT_SIZE / 2, text_width, DEFAULT_FONT_SIZE, White),
                __FUNCTION__);
        checkDraw(tumDrawText(str, SCREEN_WIDTH / 4 - text_width / 2,
                SCREEN_HEIGHT / 2 - DEFAULT_FONT_SIZE / 2, Black),
                __FUNCTION__);
        xSemaphoreGive(ScreenLock);
    }
}

void vTask4(void *pvParameters)
{
    TickType_t last_change;
    last_change = xTaskGetTickCount();

    static char str[5] = { 0 };
    static int text_width;

    sprintf(str, "%d", solution3.n4);
    text_width = tumGetTextSize((char *)str, &text_width, NULL);

    prints("Task 4 init'd\n");

    while (1) {
        xSemaphoreTake(DrawSignal, portMAX_DELAY);
        if (ulTaskNotifyTake(pdFALSE, portMAX_DELAY) == pdTRUE) {
            if (xTaskGetTickCount() - last_change > STATE_DEBOUNCE_DELAY) {
                xSemaphoreTake(solution3.lock, portMAX_DELAY);
                solution3.n4++;
                xSemaphoreGive(solution3.lock);
                sprintf(str, "%d", solution3.n4);
                tumGetTextSize((char *)str, &text_width, NULL);
                last_change = xTaskGetTickCount();
            }

            xSemaphoreTake(ScreenLock, portMAX_DELAY);
            checkDraw(tumDrawFilledBox(SCREEN_WIDTH *3/4 - text_width / 2,
                    SCREEN_HEIGHT / 2 - DEFAULT_FONT_SIZE / 2, text_width, DEFAULT_FONT_SIZE, White),
                    __FUNCTION__);
            checkDraw(tumDrawText(str, SCREEN_WIDTH *3/4 - text_width / 2,
                    SCREEN_HEIGHT / 2 - DEFAULT_FONT_SIZE / 2, Black),
                    __FUNCTION__);
            xSemaphoreGive(ScreenLock);
        }   
    }
}

void vTask5(void *pvParameters)
{
    TickType_t xLastWakeTime;
    xLastWakeTime = xTaskGetTickCount();

    static char str[5] = { 0 };
    static int text_width;

    sprintf(str, "%d", solution3.n5);
    text_width = tumGetTextSize((char *)str, &text_width, NULL);

    prints("Task 5 init'd\n");

    while (1) {
        xSemaphoreTake(DrawSignal, portMAX_DELAY);
        
        vTaskDelayUntil(&xLastWakeTime, pdMS_TO_TICKS(1000));
        if (xTaskGetTickCount() - xLastWakeTime - pdMS_TO_TICKS(1000) > 1000)
            xLastWakeTime = xTaskGetTickCount();

        xSemaphoreTake(solution3.lock, portMAX_DELAY);
        solution3.n5++;
        xSemaphoreGive(solution3.lock);

        sprintf(str, "%d", solution3.n5);
        tumGetTextSize((char *)str, &text_width, NULL);
        xSemaphoreTake(ScreenLock, portMAX_DELAY);
        checkDraw(tumDrawFilledBox(SCREEN_WIDTH / 2 - text_width / 2,
                SCREEN_HEIGHT / 2 - DEFAULT_FONT_SIZE / 2, text_width, DEFAULT_FONT_SIZE, White),
                __FUNCTION__);
        checkDraw(tumDrawText(str, SCREEN_WIDTH / 2 - text_width / 2,
                SCREEN_HEIGHT / 2 - DEFAULT_FONT_SIZE / 2, Black),
                __FUNCTION__);
        xSemaphoreGive(ScreenLock);
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

    //Load a second font for fun
    tumFontLoadFont(FPS_FONT, DEFAULT_FONT_SIZE);

    //Semaphores/Mutexes
    SyncSignal = xSemaphoreCreateBinary();
    if(!SyncSignal) {
        PRINT_ERROR("Failed to create buttons lock");
        goto err_sync_signal;
    }

    buttons.lock = xSemaphoreCreateMutex(); // Locking mechanism
    if (!buttons.lock) {
        PRINT_ERROR("Failed to create buttons lock");
        goto err_buttons_lock;
    }

    solution3.lock = xSemaphoreCreateMutex();
    if (!solution3.lock) {
        PRINT_ERROR("Failed to create solution3 lock");
        goto err_solution3_lock;
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

    //Queues
    StateQueue = xQueueCreate(STATE_QUEUE_LENGTH, sizeof(unsigned char));
    if (!StateQueue) {
        PRINT_ERROR("Could not open state queue");
        goto err_state_queue;
    }

    CurrentStateQueue = xQueueCreate(STATE_QUEUE_LENGTH, sizeof(unsigned char));
    if (!CurrentStateQueue) {
        PRINT_ERROR("Could not open current state queue");
        goto err_current_state_queue;
    }

    Task5State = xQueueCreate(STATE_QUEUE_LENGTH, sizeof(unsigned char));
    if (!Task5State) {
        PRINT_ERROR("Could not open task5 state queue");
        goto err_task5_state;
    }

    //Timers
    
    xTimers = xTimerCreate("Timer", pdMS_TO_TICKS(15000), pdTRUE, (void *) 0, vTimerCallback);
    if (xTimers == NULL) {
        PRINT_ERROR("Could not open software timers");
        goto err_timer;
    }
    

    //Infrastructure Tasks
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

    if (xTaskCreate(vSolutionSwaper, "SolutionSwaper",
                    mainGENERIC_STACK_SIZE * 2, NULL, configMAX_PRIORITIES - 3,
                    SolutionSwaper) != pdPASS) {
        PRINT_TASK_ERROR("SolutionSwaper");
        goto err_solutionswaper;
    }

    if (xTaskCreate(vReseter, "Reseter",
                    mainGENERIC_STACK_SIZE * 2, NULL, mainGENERIC_PRIORITY + 1,
                    Reseter) != pdPASS) {
        PRINT_TASK_ERROR("Reseter");
        goto err_reseter;
    }

    //Normal Tasks
    if (xTaskCreate(vTask1, "Task1", mainGENERIC_STACK_SIZE * 2,
                    NULL, mainGENERIC_PRIORITY + 3, &Task1) != pdPASS) {
        PRINT_TASK_ERROR("Task1");
        goto err_task1;
    }

    Task2 = xTaskCreateStatic(vTask2, "Task2", STACK_SIZE,
                    NULL, mainGENERIC_PRIORITY + 2, xStack, &Task2Buffer);
    if (Task2 == NULL) {
        PRINT_TASK_ERROR("Task2");
        goto err_task2;
    }

    if (xTaskCreate(vTask3, "Task3", mainGENERIC_STACK_SIZE * 2,
                    NULL, mainGENERIC_PRIORITY + 2, &Task3) != pdPASS) {
        PRINT_TASK_ERROR("Task3");
        goto err_task3;
    }

    if (xTaskCreate(vTask4, "Task4", mainGENERIC_STACK_SIZE * 2,
                    NULL, mainGENERIC_PRIORITY + 3, &Task4) != pdPASS) {
        PRINT_TASK_ERROR("Task4");
        goto err_task4;
    }

    if (xTaskCreate(vTask5, "Task5", mainGENERIC_STACK_SIZE * 2,
                    NULL, mainGENERIC_PRIORITY + 2, &Task5) != pdPASS) {
        PRINT_TASK_ERROR("Task4");
        goto err_task5;
    }

    vTaskSuspend(Task1);
    vTaskSuspend(Task2);
    vTaskSuspend(Task3);
    vTaskSuspend(Task4);
    vTaskSuspend(Task5);
    //vTaskSuspend(Reseter);

    vTaskStartScheduler();

    return EXIT_SUCCESS;

    err_task5:
        vTaskDelete(Task4);
    err_task4:
        vTaskDelete(Task3);
    err_task3:
        vTaskDelete(Task2);
    err_task2:
        vTaskDelete(Task1);
    err_task1:
        vTaskDelete(Reseter);
    err_reseter:
        vTaskDelete(SolutionSwaper);
    err_solutionswaper:
        vTaskDelete(BufferSwap);
    err_bufferswap:
        vTaskDelete(StateMachine);
    err_statemachine:
        xTimerDelete(xTimers, 0);
    err_timer:
        vQueueDelete(Task5State);
    err_task5_state:
        vQueueDelete(CurrentStateQueue);
    err_current_state_queue:
        vQueueDelete(StateQueue);
    err_state_queue:
        vSemaphoreDelete(ScreenLock);
    err_screen_lock:
        vSemaphoreDelete(DrawSignal);
    err_draw_signal:
        vSemaphoreDelete(solution3.lock);
    err_solution3_lock:    
        vSemaphoreDelete(buttons.lock);
    err_buttons_lock:
        vSemaphoreDelete(SyncSignal);
    err_sync_signal:
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
