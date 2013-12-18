/* -*- C++ -*-

This file implements the WeaverImpl class.


$ Author: Mirko Boehm $
$ Copyright: (C) 2005-2013 Mirko Boehm $
$ Contact: mirko@kde.org
http://www.kde.org
http://creative-destruction.me $

   This library is free software; you can redistribute it and/or
   modify it under the terms of the GNU Library General Public
   License as published by the Free Software Foundation; either
   version 2 of the License, or (at your option) any later version.

   This library is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
   Library General Public License for more details.

   You should have received a copy of the GNU Library General Public License
   along with this library; see the file COPYING.LIB.  If not, write to
   the Free Software Foundation, Inc., 51 Franklin Street, Fifth Floor,
   Boston, MA 02110-1301, USA.

$Id: WeaverImpl.cpp 30 2005-08-16 16:16:04Z mirko $

*/

#include "WeaverImpl.h"

#include <QtCore/QCoreApplication>
#include <QtCore/QObject>
#include <QtCore/QMutex>
#include <QtCore/QDebug>

#include "Job.h"
#include "ManagedJobPointer.h"
#include "State.h"
#include "Thread.h"
#include "ThreadWeaver.h"
#include "DebuggingAids.h"
#include "WeaverObserver.h"
#include "SuspendedState.h"
#include "SuspendingState.h"
#include "DestructedState.h"
#include "WorkingHardState.h"
#include "ShuttingDownState.h"
#include "InConstructionState.h"
#include "QueuePolicy.h"

using namespace ThreadWeaver;

WeaverImpl::WeaverImpl( QObject* parent )
    : QueueAPI(parent)
    , m_active(0)
    , m_inventoryMax( qMax(4, 2 * QThread::idealThreadCount() ) )
    , m_mutex ( new QMutex( QMutex::NonRecursive ) )
{
    qRegisterMetaType<ThreadWeaver::JobPointer>("ThreadWeaver::JobPointer");

    QMutexLocker l(m_mutex); Q_UNUSED(l);
    // initialize state objects:
    m_states[InConstruction] = QSharedPointer<State>(new InConstructionState(this));
    setState_p(InConstruction);
    m_states[WorkingHard] = QSharedPointer<State>(new WorkingHardState(this));
    m_states[Suspending] = QSharedPointer<State>(new SuspendingState(this));
    m_states[Suspended] = QSharedPointer<State>(new SuspendedState(this));
    m_states[ShuttingDown] = QSharedPointer<State>(new ShuttingDownState(this));
    m_states[Destructed] = QSharedPointer<State>(new DestructedState(this));
    setState_p(WorkingHard);
}

WeaverImpl::~WeaverImpl()
{
    Q_ASSERT_X(state()->stateId() == Destructed, Q_FUNC_INFO, "shutDown() method was not called before WeaverImpl destructor!");
    delete m_mutex;
}

void WeaverImpl::shutDown()
{
    state()->shutDown();
}

void WeaverImpl::shutDown_p()
{
    // the constructor may only be called from the thread that owns this
    // object (everything else would be what we professionals call "insane")

    REQUIRE( QThread::currentThread() == thread() );
    debug ( 3, "WeaverImpl::shutDown: destroying inventory.\n" );
    m_semaphore.acquire(m_createdThreads.loadAcquire());
    finish();
    suspend();
    setState(ShuttingDown);
    reschedule();
    m_jobFinished.wakeAll();

    // problem: Some threads might not be asleep yet, just finding
    // out if a job is available. Those threads will suspend
    // waiting for their next job (a rare case, but not impossible).
    // Therefore, if we encounter a thread that has not exited, we
    // have to wake it again (which we do in the following for
    // loop).

    for (;;) {
        Thread* th = 0;
        {
            QMutexLocker l(m_mutex); Q_UNUSED(l);
            if (m_inventory.isEmpty()) break;
            th = m_inventory.takeFirst();
        }
        if ( !th->isFinished() )
        {
            for ( ;; )
            {
                Q_ASSERT(state()->stateId() == ShuttingDown);
                reschedule();
                if ( th->wait( 100 ) ) break;
                debug ( 1,  "WeaverImpl::shutDown: thread %i did not exit as expected, "
                        "retrying.\n", th->id() );
            }
        }
        emit ( threadExited ( th ) );
        delete th;
    }
    Q_ASSERT(m_inventory.isEmpty());
    debug ( 3, "WeaverImpl::shutDown: done\n" );
    setState ( Destructed ); // Destructed ignores all calls into the queue API
}

void WeaverImpl::setState ( StateId id )
{
    QMutexLocker l(m_mutex); Q_UNUSED(l);
    setState_p(id);
}

void WeaverImpl::setState_p(StateId id)
{
    Q_ASSERT(!m_mutex->tryLock()); //mutex has to be held when this method is called
    State* newState = m_states[id].data();
    State* previous = m_state.fetchAndStoreOrdered(newState);
    if (previous==0 || previous->stateId() != id) {
        newState->activated();
        debug(2, "WeaverImpl::setState: state changed to \"%s\".\n", newState->stateName().toLatin1().constData());
        if (id == Suspended) {
            emit(suspended());
        }
        emit(stateChanged(newState));
    }
}

const State *WeaverImpl::state() const
{
    return m_state.loadAcquire();
}

State* WeaverImpl::state()
{
    return m_state.loadAcquire();
}


void WeaverImpl::setMaximumNumberOfThreads( int cap )
{
    //FIXME really? Why not 0?
    Q_ASSERT_X ( cap > 0, "Weaver Impl", "Thread inventory size has to be larger than zero." );
    QMutexLocker l (m_mutex);  Q_UNUSED(l);
    state()->setMaximumNumberOfThreads(cap);
}

void WeaverImpl::setMaximumNumberOfThreads_p(int cap)
{
    Q_ASSERT(!m_mutex->tryLock()); //mutex has to be held when this method is called
    m_inventoryMax = cap;
}

int WeaverImpl::maximumNumberOfThreads() const
{
    QMutexLocker l(m_mutex); Q_UNUSED(l);
    return state()->maximumNumberOfThreads();
}

int WeaverImpl::maximumNumberOfThreads_p() const
{
    Q_ASSERT(!m_mutex->tryLock()); //mutex has to be held when this method is called
    return m_inventoryMax;
}

int WeaverImpl::currentNumberOfThreads () const
{
    QMutexLocker l(m_mutex); Q_UNUSED(l);
    return state()->currentNumberOfThreads();
}

int WeaverImpl::currentNumberOfThreads_p() const
{
    Q_ASSERT(!m_mutex->tryLock()); //mutex has to be held when this method is called
    return m_inventory.count();
}

void WeaverImpl::registerObserver ( WeaverObserver *ext )
{
    QMutexLocker l(m_mutex); Q_UNUSED(l);
    return state()->registerObserver(ext);
}

void WeaverImpl::registerObserver_p(WeaverObserver *ext)
{
    //FIXME test and fix, it is broken!
    connect(this, SIGNAL (stateChanged(ThreadWeaver::State*)),
            ext, SIGNAL (weaverStateChanged(ThreadWeaver::State*)));
    connect(this,  SIGNAL (threadStarted(ThreadWeaver::Thread*)),
            ext,  SIGNAL (threadStarted(ThreadWeaver::Thread*)));
    connect(this,  SIGNAL (threadBusy(ThreadWeaver::Thread*,ThreadWeaver::Job*)),
            ext,  SIGNAL (threadBusy(ThreadWeaver::Thread*,ThreadWeaver::Job*)));
    connect(this,  SIGNAL (threadSuspended(ThreadWeaver::Thread*)),
            ext,  SIGNAL (threadSuspended(ThreadWeaver::Thread*)));
    connect(this,  SIGNAL (threadExited(ThreadWeaver::Thread*)) ,
            ext,  SIGNAL (threadExited(ThreadWeaver::Thread*)));
}

void WeaverImpl::enqueue(const QVector<JobPointer>& jobs)
{
    QMutexLocker l(m_mutex); Q_UNUSED(l);
    state()->enqueue(jobs);
}

void WeaverImpl::enqueue_p(const QVector<JobPointer>& jobs)
{
    Q_ASSERT(!m_mutex->tryLock()); //mutex has to be held when this method is called
    if (jobs.isEmpty()) {
        return;
    }
    Q_FOREACH(const JobPointer& job, jobs) {
        if (job) {
            Q_ASSERT(job->status() == Job::Status_New);
            adjustInventory(1);
            debug(3, "WeaverImpl::enqueue: queueing job %p.\n", (void*)job.data());
            job->aboutToBeQueued(this);
            // find position for insertion:
            int i = m_assignments.size();
            if (i > 0) {
                while(i > 0 && m_assignments.at(i - 1)->priority() < job->priority()) --i;
                m_assignments.insert(i, job);
            } else {
                m_assignments.append(job);
            }
            job->setStatus(Job::Status_Queued);
            reschedule();
        }
    }
}

bool WeaverImpl::dequeue(const JobPointer &job)
{
    QMutexLocker l(m_mutex); Q_UNUSED(l);
    return state()->dequeue(job);
}

bool WeaverImpl::dequeue_p(JobPointer job)
{
    Q_ASSERT(!m_mutex->tryLock()); //mutex has to be held when this method is called
    int position = m_assignments.indexOf(job);
    if (position != -1) {
        job->aboutToBeDequeued(this);
        int newPosition = m_assignments.indexOf(job);
        JobPointer job = m_assignments.takeAt(newPosition);
        job->setStatus(Job::Status_New);
        Q_ASSERT(!m_assignments.contains(job));
        debug(3, "WeaverImpl::dequeue: job %p dequeued, %i jobs left.\n", (void*)job.data(), queueLength_p());
        // from the queues point of view, a job is just as finished if it gets dequeued:
        m_jobFinished.wakeAll();
        Q_ASSERT(!m_assignments.contains(job));
        return true;
    } else {
        debug( 3, "WeaverImpl::dequeue: job %p not found in queue.\n", (void*)job.data() );
        return false;
    }
}

void WeaverImpl::dequeue ()
{
    QMutexLocker l(m_mutex); Q_UNUSED(l);
    state()->dequeue();
}

void WeaverImpl::dequeue_p()
{
    Q_ASSERT(!m_mutex->tryLock()); //mutex has to be held when this method is called
    debug( 3, "WeaverImpl::dequeue: dequeueing all jobs.\n" );
    for ( int index = 0; index < m_assignments.size(); ++index ) {
        m_assignments.at( index )->aboutToBeDequeued( this );
    }
    m_assignments.clear();
    ENSURE ( m_assignments.isEmpty() );
}

void WeaverImpl::finish()
{
    QMutexLocker l(m_mutex); Q_UNUSED(l);
    state()->finish();
}

void WeaverImpl::finish_p()
{
    Q_ASSERT(!m_mutex->tryLock()); //mutex has to be held when this method is called
#ifdef QT_NO_DEBUG
    const int MaxWaitMilliSeconds = 50;
#else
    const int MaxWaitMilliSeconds = 500;
#endif
    while (!isIdle_p()) {
        Q_ASSERT_X(state()->stateId() == WorkingHard, Q_FUNC_INFO, qPrintable(state()->stateName()));
        debug(2, "WeaverImpl::finish: not done, waiting.\n" );
        if (m_jobFinished.wait(m_mutex, MaxWaitMilliSeconds) == false) {
            debug(2, "WeaverImpl::finish: wait timed out, %i jobs left, waking threads.\n", queueLength_p());
            reschedule();
        }
    }
    debug (2, "WeaverImpl::finish: done.\n\n\n" );
}

void WeaverImpl::suspend ()
{
    //FIXME?
    //QMutexLocker l(m_mutex); Q_UNUSED(l);
    state()->suspend();
}

void WeaverImpl::suspend_p()
{
    //FIXME ?
}

void WeaverImpl::resume ( )
{
    //FIXME?
    //QMutexLocker l(m_mutex); Q_UNUSED(l);
    state()->resume();
}

void WeaverImpl::resume_p()
{
    //FIXME ?
}

bool WeaverImpl::isEmpty() const
{
    QMutexLocker l(m_mutex); Q_UNUSED(l);
    return state()->isEmpty();
}

bool WeaverImpl::isEmpty_p() const
{
    Q_ASSERT(!m_mutex->tryLock()); //mutex has to be held when this method is called
    return  m_assignments.isEmpty();
}

bool WeaverImpl::isIdle() const
{
    QMutexLocker l(m_mutex); Q_UNUSED(l);
    return state()->isIdle();
}

bool WeaverImpl::isIdle_p() const
{
    Q_ASSERT(!m_mutex->tryLock()); //mutex has to be held when this method is called
    return isEmpty_p() && m_active == 0;
}

int WeaverImpl::queueLength() const
{
    QMutexLocker l(m_mutex); Q_UNUSED(l);
    return state()->queueLength();
}

int WeaverImpl::queueLength_p() const
{
    Q_ASSERT(!m_mutex->tryLock()); //mutex has to be held when this method is called
    return m_assignments.count();
}

void WeaverImpl::requestAbort()
{
    QMutexLocker l(m_mutex); Q_UNUSED(l);
    return state()->requestAbort();
}

void WeaverImpl::reschedule()
{
    m_jobAvailable.wakeAll();
}

void WeaverImpl::requestAbort_p()
{
    Q_ASSERT(!m_mutex->tryLock()); //mutex has to be held when this method is called
    for ( int i = 0; i<m_inventory.size(); ++i ) {
        m_inventory[i]->requestAbort();
    }
}

void WeaverImpl::adjustInventory ( int numberOfNewJobs )
{
    Q_ASSERT(!m_mutex->tryLock()); //mutex has to be held when this method is called
    //number of threads that can be created:
    const int reserve = m_inventoryMax - m_inventory.count();

    if (reserve > 0) {
        for (int i = 0; i < qMin(reserve, numberOfNewJobs); ++i) {
            Thread *th = createThread();
            th->moveToThread( th ); // be sane from the start
            m_inventory.append(th);
            connect(th, SIGNAL(jobStarted(ThreadWeaver::JobPointer,ThreadWeaver::Thread*)),
                    SIGNAL (threadBusy(ThreadWeaver::JobPointer,ThreadWeaver::Thread*)));
            connect(th, SIGNAL(jobDone(ThreadWeaver::JobPointer)),
                    SIGNAL (jobDone(ThreadWeaver::JobPointer)));
            th->start();
            m_createdThreads.ref();
            debug(2, "WeaverImpl::adjustInventory: thread created, "
                  "%i threads in inventory.\n", currentNumberOfThreads_p());
        }
    }
}

bool WeaverImpl::canBeExecuted(JobPointer job)
{
    Q_ASSERT(!m_mutex->tryLock()); //mutex has to be held when this method is called

    QList<QueuePolicy*> acquired;

    bool success = true;

    QList<QueuePolicy*> policies = job->queuePolicies();
    if (!policies.isEmpty()) {
        debug(4, "WeaverImpl::canBeExecuted: acquiring permission from %i queue %s.\n",
              policies.size(), policies.size()==1 ? "policy" : "policies" );
        for (int index = 0; index < policies.size(); ++index) {
            if (policies.at(index)->canRun(job)) {
                acquired.append(policies.at(index));
            } else {
                success = false;
                break;
            }
        }

        debug(4, "WeaverImpl::canBeExecuted: queue policies returned %s.\n", success ? "true" : "false");

        if (!success) {
            for (int index = 0; index < acquired.size(); ++index) {
                acquired.at(index)->release(job);
            }
        }
    } else {
        debug(4, "WeaverImpl::canBeExecuted: no queue policies, this job can be executed.\n");
    }
    return success;
}

Thread* WeaverImpl::createThread()
{
    return new Thread( this );
}

void WeaverImpl::incActiveThreadCount()
{
    adjustActiveThreadCount ( 1 );
}

void WeaverImpl::decActiveThreadCount()
{
    adjustActiveThreadCount(-1);
    // the done job could have freed another set of jobs, and we do not know how
    // many - therefore we need to wake all threads:
    m_jobFinished.wakeAll();
}

void WeaverImpl::adjustActiveThreadCount(int diff)
{
    Q_ASSERT(!m_mutex->tryLock()); //mutex has to be held when this method is called
    m_active += diff;
    debug(4, "WeaverImpl::adjustActiveThreadCount: %i active threads (%i jobs"
          " in queue).\n", m_active,  queueLength_p());

    if (m_assignments.isEmpty() && m_active == 0) {
        P_ASSERT ( diff < 0 ); // cannot reach zero otherwise
        emit ( finished() );
    }
}

int WeaverImpl::activeThreadCount()
{
    Q_ASSERT(!m_mutex->tryLock()); //mutex has to be held when this method is called
    return m_active;
}

void WeaverImpl::threadEnteredRun(Thread *thread)
{
    m_semaphore.release(1);
    emit threadStarted(thread);
}

JobPointer WeaverImpl::takeFirstAvailableJobOrSuspendOrWait(Thread *th, bool threadWasBusy,
                                                            bool suspendIfInactive, bool justReturning)
{
    QMutexLocker l (m_mutex); Q_UNUSED(l);
    Q_ASSERT(threadWasBusy==false || (threadWasBusy == true && m_active > 0));
    debug(3, "WeaverImpl::takeFirstAvailableJobOrWait: trying to assign new job to thread %i (%s state).\n",
          th->id(), qPrintable(state()->stateName()));
    debug(5, "WeaverImpl::takeFirstAvailableJobOrWait: %i active threads, was busy: %s, suspend: %s, assign new job: %s.\n",
          activeThreadCount(), threadWasBusy ? "yes" : "no", suspendIfInactive ? "yes" : "no", !justReturning ? "yes" : "no");
    if (threadWasBusy) {
        // cleanup and send events:
        decActiveThreadCount();
    }
    Q_ASSERT(m_active >= 0);

    if (suspendIfInactive && m_active == 0 && state()->stateId() == Suspending) {
        setState_p(Suspended);
        return JobPointer();
    }

    if (state()->stateId() != WorkingHard || justReturning) {
        return JobPointer();
    }

    JobPointer next;
    for (int index = 0; index < m_assignments.size(); ++index) {
        const JobPointer& candidate = m_assignments.at(index);
        if (canBeExecuted(candidate)) {
            next = candidate;
            m_assignments.removeAt (index);
            break;
        }
    }
    if (next) {
        incActiveThreadCount();
        debug(3, "WeaverImpl::takeFirstAvailableJobOrWait: job %p assigned to thread %i (%s state).\n",
              next.data(), th->id(), qPrintable(state()->stateName()));
        return next;
    }

    blockThreadUntilJobsAreBeingAssigned_locked(th);
    return JobPointer();
}

JobPointer WeaverImpl::applyForWork(Thread *th, bool wasBusy)
{
    return state()->applyForWork(th, wasBusy);
}

void WeaverImpl::waitForAvailableJob(Thread* th)
{
    state()->waitForAvailableJob(th);
}

void WeaverImpl::blockThreadUntilJobsAreBeingAssigned(Thread *th)
{
    QMutexLocker l(m_mutex); Q_UNUSED(l);
    blockThreadUntilJobsAreBeingAssigned_locked(th);
}

void WeaverImpl::blockThreadUntilJobsAreBeingAssigned_locked(Thread *th)
{
    Q_ASSERT(!m_mutex->tryLock()); //mutex has to be held when this method is called
    debug(4, "WeaverImpl::blockThreadUntilJobsAreBeingAssigned_locked: thread %i blocked (%s state).\n",
          th->id(), qPrintable(state()->stateName()));
    emit threadSuspended(th);
    m_jobAvailable.wait(m_mutex);
    debug(4, "WeaverImpl::blockThreadUntilJobsAreBeingAssigned_locked: thread %i resumed  (%s state).\n",
          th->id(), qPrintable(state()->stateName()));
}

void WeaverImpl::dumpJobs()
{
    QMutexLocker l(m_mutex); Q_UNUSED(l);
    debug( 0, "WeaverImpl::dumpJobs: current jobs:\n" );
    for ( int index = 0; index < m_assignments.size(); ++index ) {
        debug( 0, "--> %4i: %p (priority %i, can be executed: %s)\n", index, (void*)m_assignments.at( index ).data(),
               m_assignments.at(index)->priority(),
               canBeExecuted(m_assignments.at(index)) ? "yes" : "no");
    }
}

