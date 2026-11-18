#pragma once
#include "tasks_adapter.h"
namespace pulse::extensions::native {
TasksBackend MakeTasksBackend(HWND owner);
}
