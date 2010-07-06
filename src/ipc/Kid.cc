/*
 * $Id$
 *
 * DEBUG: section 54    Interprocess Communication
 *
 */

#include "config.h"
#include "ipc/Kid.h"

Kid::Kid():
        badFailures(0),
        pid(-1),
        startTime(0),
        isRunning(false)
{
}

Kid::Kid(const String& kid_name):
        theName(kid_name),
        badFailures(0),
        pid(-1),
        startTime(0),
        isRunning(false)
{
}

/// called when this kid got started, records PID
void Kid::start(pid_t cpid)
{
    assert(!running());
    assert(cpid > 0);

    isRunning = true;
    pid = cpid;
    time(&startTime);
}

/// called when kid terminates, sets exiting status
void Kid::stop(status_type exitStatus)
{
    assert(running());
    assert(startTime != 0);

    isRunning = false;

    time_t stop_time;
    time(&stop_time);
    if ((stop_time - startTime) < fastFailureTimeLimit)
        badFailures++;
    else
        badFailures = 0; // the failures are not "frequent" [any more]

    status = exitStatus;
}

/// returns true if tracking of kid is stopped
bool Kid::running() const
{
    return isRunning;
}

/// returns current pid for a running kid and last pid for a stopped kid
pid_t Kid::getPid() const
{
    assert(pid > 0);
    return pid;
}

/// whether the failures are "repeated and frequent"
bool Kid::hopeless() const
{
    return badFailures > badFailureLimit;
}

/// returns true if the process terminated normally
bool Kid::calledExit() const
{
    return (pid > 0) && !running() && WIFEXITED(status);
}

/// returns the exit status of the process
int Kid::exitStatus() const
{
    return WEXITSTATUS(status);
}

/// whether the process exited with a given exit status code
bool Kid::calledExit(int code) const
{
    return calledExit() && (exitStatus() == code);
}

/// whether the process exited with code 0
bool Kid::exitedHappy() const
{
    return calledExit(0);
}

/// returns true if the kid was terminated by a signal
bool Kid::signaled() const
{
    return (pid > 0) && !running() && WIFSIGNALED(status);
}

/// returns the number of the signal that caused the kid to terminate
int Kid::termSignal() const
{
    return WTERMSIG(status);
}

/// whether the process was terminated by a given signal
bool Kid::signaled(int sgnl) const
{
    return signaled() && (termSignal() == sgnl);
}

/// returns kid name
const String& Kid::name() const
{
    return theName;
}
