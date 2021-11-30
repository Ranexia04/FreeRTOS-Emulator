Git tutorial on https://github.com/Ranexia04/git-tutorial

Press E to switch states.
While in state 1: press left mouse button to reset the values.

While in state 2: press 1 and 2 to increment the numbers on the screen (on the top and bottom). press 3 to stop/start the number that increments every second.

Regarding state 3: In my computer it works a majoraty of the time but not always. (i.e. my hypothesis is that sometimes it takes too long for the task to execute and the tick increments, therefore they get put on the next tick on the screen.). When i lower the tick rate it works better but makes the rest of the program more clunky.

Q. What is a kernel tick?
A. The hardware crystal counts every x ammount of time. When this counting reaches a certain number (defined by the user, e.g. if tick rate == 1000, then it should count to 1ms.), the task scheduler is invoked so it can determine what task should be scheduled next.

Q. What is a tickless kernel?
A. On this type of kernel there are no ticks as the name implies, which means that the system will not invoke the scheduler periodically. Only when a task gets blocked or suspended will the task be invoked to know which task should run next.

Q. What happens when the stack size is too low?
A. Even when stack size is 0 nothing happens.

Q. What happens when priority gets changed?
A. The tick when the tasks get called remains the same but the order of the numbers on the screen becomes different. However the priority of task 3 does not realy matter since it only get called after task 2 gets called no matter what.