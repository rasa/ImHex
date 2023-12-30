#include <hex/api/task_manager.hpp>

#include <hex/api/localization_manager.hpp>
#include <hex/helpers/logger.hpp>

#include <algorithm>

#include <jthread.hpp>

#if defined(OS_WINDOWS)
    #include <windows.h>
    #include <processthreadsapi.h>
#else
    #include <pthread.h>
#endif

namespace hex {

    namespace {

        std::mutex s_deferredCallsMutex, s_tasksFinishedMutex;

        std::list<std::shared_ptr<Task>> s_tasks, s_taskQueue;
        std::list<std::function<void()>> s_deferredCalls;
        std::list<std::function<void()>> s_tasksFinishedCallbacks;

        std::mutex s_queueMutex;
        std::condition_variable s_jobCondVar;
        std::vector<std::jthread> s_workers;

    }


    static void setThreadName(const std::string &name) {
        #if defined(OS_WINDOWS)
            typedef struct tagTHREADNAME_INFO {
                DWORD dwType;
                LPCSTR szName;
                DWORD dwThreadID;
                DWORD dwFlags;
            } THREADNAME_INFO;

            THREADNAME_INFO info;
            info.dwType = 0x1000;
            info.szName = name.c_str();
            info.dwThreadID = ::GetCurrentThreadId();
            info.dwFlags = 0;

            constexpr static DWORD MS_VC_EXCEPTION = 0x406D1388;
            RaiseException(MS_VC_EXCEPTION, 0, sizeof(info) / sizeof(ULONG_PTR), reinterpret_cast<ULONG_PTR*>(&info));
        #elif defined(OS_LINUX)
            pthread_setname_np(pthread_self(), name.c_str());
        #elif defined(OS_WEB)
            hex::unused(name);
        #elif defined(OS_MACOS)
            pthread_setname_np(name.c_str());
        #endif
    }

    Task::Task(UnlocalizedString unlocalizedName, u64 maxValue, bool background, std::function<void(Task &)> function)
    : m_unlocalizedName(std::move(unlocalizedName)), m_maxValue(maxValue), m_function(std::move(function)), m_background(background) { }

    Task::Task(hex::Task &&other) noexcept {
        {
            std::scoped_lock thisLock(m_mutex);
            std::scoped_lock otherLock(other.m_mutex);

            m_function = std::move(other.m_function);
            m_unlocalizedName = std::move(other.m_unlocalizedName);
        }

        m_maxValue    = u64(other.m_maxValue);
        m_currValue   = u64(other.m_currValue);

        m_finished        = bool(other.m_finished);
        m_hadException    = bool(other.m_hadException);
        m_interrupted     = bool(other.m_interrupted);
        m_shouldInterrupt = bool(other.m_shouldInterrupt);
    }

    Task::~Task() {
        if (!this->isFinished())
            this->interrupt();
    }

    void Task::update(u64 value) {
        // Update the current progress value of the task
        m_currValue.store(value, std::memory_order_relaxed);

        // Check if the task has been interrupted by the main thread and if yes,
        // throw an exception that is generally not caught by the task
        if (m_shouldInterrupt.load(std::memory_order_relaxed)) [[unlikely]]
            throw TaskInterruptor();
    }

    void Task::setMaxValue(u64 value) {
        m_maxValue = value;
    }


    void Task::interrupt() {
        m_shouldInterrupt = true;

        // Call the interrupt callback on the current thread if one is set
        if (m_interruptCallback)
            m_interruptCallback();
    }

    void Task::setInterruptCallback(std::function<void()> callback) {
        m_interruptCallback = std::move(callback);
    }

    bool Task::isBackgroundTask() const {
        return m_background;
    }

    bool Task::isFinished() const {
        return m_finished;
    }

    bool Task::hadException() const {
        return m_hadException;
    }

    bool Task::shouldInterrupt() const {
        return m_shouldInterrupt;
    }

    bool Task::wasInterrupted() const {
        return m_interrupted;
    }

    void Task::clearException() {
        m_hadException = false;
    }

    std::string Task::getExceptionMessage() const {
        std::scoped_lock lock(m_mutex);

        return m_exceptionMessage;
    }

    const UnlocalizedString &Task::getUnlocalizedName() {
        return m_unlocalizedName;
    }

    u64 Task::getValue() const {
        return m_currValue;
    }

    u64 Task::getMaxValue() const {
        return m_maxValue;
    }

    void Task::finish() {
        m_finished = true;
    }

    void Task::interruption() {
        m_interrupted = true;
    }

    void Task::exception(const char *message) {
        std::scoped_lock lock(m_mutex);

        // Store information about the caught exception
        m_exceptionMessage = message;
        m_hadException = true;
    }


    bool TaskHolder::isRunning() const {
        auto task = m_task.lock();
        if (!task)
            return false;

        return !task->isFinished();
    }

    bool TaskHolder::hadException() const {
        auto task = m_task.lock();
        if (!task)
            return false;

        return !task->hadException();
    }

    bool TaskHolder::shouldInterrupt() const {
        auto task = m_task.lock();
        if (!task)
            return false;

        return !task->shouldInterrupt();
    }

    bool TaskHolder::wasInterrupted() const {
        auto task = m_task.lock();
        if (!task)
            return false;

        return !task->wasInterrupted();
    }

    void TaskHolder::interrupt() const {
        auto task = m_task.lock();
        if (!task)
            return;

        task->interrupt();
    }

    u32 TaskHolder::getProgress() const {
        auto task = m_task.lock();
        if (!task)
            return false;

        // If the max value is 0, the task has no progress
        if (task->getMaxValue() == 0)
            return 0;

        // Calculate the progress of the task from 0 to 100
        return u32((task->getValue() * 100) / task->getMaxValue());
    }


    void TaskManager::init() {
        const auto threadCount = std::thread::hardware_concurrency();

        log::debug("Initializing task manager thread pool with {} workers.", threadCount);

        // Create worker threads
        for (u32 i = 0; i < threadCount; i++) {
            s_workers.emplace_back([](const std::stop_token &stopToken) {
                while (true) {
                    std::shared_ptr<Task> task;

                    // Set the thread name to "Idle Task" while waiting for a task
                    setThreadName("Idle Task");

                    {
                        // Wait for a task to be added to the queue
                        std::unique_lock lock(s_queueMutex);
                        s_jobCondVar.wait(lock, [&] {
                            return !s_taskQueue.empty() || stopToken.stop_requested();
                        });

                        // Check if the thread should exit
                        if (stopToken.stop_requested())
                            break;

                        // Grab the next task from the queue
                        task = std::move(s_taskQueue.front());
                        s_taskQueue.pop_front();
                    }

                    try {
                        // Set the thread name to the name of the task
                        setThreadName(Lang(task->m_unlocalizedName));

                        // Execute the task
                        task->m_function(*task);

                        log::debug("Task '{}' finished", task->m_unlocalizedName.get());
                    } catch (const Task::TaskInterruptor &) {
                        // Handle the task being interrupted by user request
                        task->interruption();
                    } catch (const std::exception &e) {
                        log::error("Exception in task '{}': {}", task->m_unlocalizedName.get(), e.what());

                        // Handle the task throwing an uncaught exception
                        task->exception(e.what());
                    } catch (...) {
                        log::error("Exception in task '{}'", task->m_unlocalizedName.get());

                        // Handle the task throwing an uncaught exception of unknown type
                        task->exception("Unknown Exception");
                    }

                    task->finish();
                }
            });
        }
    }

    void TaskManager::exit() {
        // Interrupt all tasks
        for (auto &task : s_tasks) {
            task->interrupt();
        }

        // Ask worker threads to exit after finishing their task
        for (auto &thread : s_workers)
            thread.request_stop();

        // Wake up all the idle worker threads so they can exit
        s_jobCondVar.notify_all();

        // Wait for all worker threads to exit
        s_workers.clear();

        s_tasks.clear();
        s_taskQueue.clear();
    }

    TaskHolder TaskManager::createTask(std::string name, u64 maxValue, bool background, std::function<void(Task&)> function) {
        std::scoped_lock lock(s_queueMutex);

        // Construct new task
        auto task = std::make_shared<Task>(std::move(name), maxValue, background, std::move(function));

        s_tasks.emplace_back(task);

        // Add task to the queue for the worker to pick up
        s_taskQueue.emplace_back(std::move(task));

        s_jobCondVar.notify_one();

        return TaskHolder(s_tasks.back());
    }


    TaskHolder TaskManager::createTask(std::string name, u64 maxValue, std::function<void(Task &)> function) {
        log::debug("Creating task {}", name);
        return createTask(std::move(name), maxValue, false, std::move(function));
    }

    TaskHolder TaskManager::createBackgroundTask(std::string name, std::function<void(Task &)> function) {
        log::debug("Creating background task {}", name);
        return createTask(std::move(name), 0, true, std::move(function));
    }

    void TaskManager::collectGarbage() {
        {
            std::scoped_lock lock(s_queueMutex);
            std::erase_if(s_tasks, [](const auto &task) {
                return task->isFinished() && !task->hadException();
            });
        }

        if (s_tasks.empty()) {
            std::scoped_lock lock(s_deferredCallsMutex);
            for (auto &call : s_tasksFinishedCallbacks)
                call();
            s_tasksFinishedCallbacks.clear();
        }

    }

    std::list<std::shared_ptr<Task>> &TaskManager::getRunningTasks() {
        return s_tasks;
    }

    size_t TaskManager::getRunningTaskCount() {
        std::scoped_lock lock(s_queueMutex);

        return std::count_if(s_tasks.begin(), s_tasks.end(), [](const auto &task){
            return !task->isBackgroundTask();
        });
    }

    size_t TaskManager::getRunningBackgroundTaskCount() {
        std::scoped_lock lock(s_queueMutex);

        return std::count_if(s_tasks.begin(), s_tasks.end(), [](const auto &task){
            return task->isBackgroundTask();
        });
    }


    void TaskManager::doLater(const std::function<void()> &function) {
        std::scoped_lock lock(s_deferredCallsMutex);

        s_deferredCalls.push_back(function);
    }

    void TaskManager::runDeferredCalls() {
        std::scoped_lock lock(s_deferredCallsMutex);

        for (const auto &call : s_deferredCalls)
            call();

        s_deferredCalls.clear();
    }

    void TaskManager::runWhenTasksFinished(const std::function<void()> &function) {
        std::scoped_lock lock(s_tasksFinishedMutex);

        s_tasksFinishedCallbacks.push_back(function);
    }

}
