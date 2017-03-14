/*
 * NDPluginDriver.cpp
 * 
 * Asyn driver for callbacks to save area detector data to files.
 *
 * Author: Mark Rivers
 *
 * Created April 5, 2008
 */

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <errno.h>

#include <epicsTypes.h>
#include <epicsMessageQueue.h>
#include <epicsThread.h>
#include <epicsEvent.h>
#include <epicsTime.h>
#include <cantProceed.h>

#include <asynDriver.h>

#include <epicsExport.h>
#include "NDPluginDriver.h"

typedef enum {
    ToThreadMessageData,
    ToThreadMessageExit
} ToThreadMessageType_t;

typedef struct {
    ToThreadMessageType_t messageType;
    NDArray *pArray;    
} ToThreadMessage_t;

typedef enum {
    FromThreadMessageEnter,
    FromThreadMessageExit
} FromThreadMessageType_t;

typedef struct {
    FromThreadMessageType_t messageType;
    epicsThreadId threadId;
} FromThreadMessage_t;

static const char *driverName="NDPluginDriver";

sortedListElement::sortedListElement(NDArray *pArray, epicsTimeStamp time)
    : pArray_(pArray), insertionTime_(time) {}

static void sortingTaskC(void *drvPvt)
{
    NDPluginDriver *pPvt = (NDPluginDriver *)drvPvt;

    pPvt->sortingTask();
}

/** Constructor for NDPluginDriver; most parameters are simply passed to asynNDArrayDriver::asynNDArrayDriver.
  * After calling the base class constructor this method creates a thread to execute the NDArray callbacks, 
  * and sets reasonable default values for all of the parameters defined in NDPluginDriver.h.
  * \param[in] portName The name of the asyn port driver to be created.
  * \param[in] queueSize The number of NDArrays that the input queue for this plugin can hold when 
  *            NDPluginDriverBlockingCallbacks=0.  Larger queues can decrease the number of dropped arrays,
  *            at the expense of more NDArray buffers being allocated from the underlying driver's NDArrayPool.
  * \param[in] blockingCallbacks Initial setting for the NDPluginDriverBlockingCallbacks flag.
  *            0=callbacks are queued and executed by the callback thread; 1 callbacks execute in the thread
  *            of the driver doing the callbacks.
  * \param[in] NDArrayPort Name of asyn port driver for initial source of NDArray callbacks.
  * \param[in] NDArrayAddr asyn port driver address for initial source of NDArray callbacks.
  * \param[in] maxAddr The maximum  number of asyn addr addresses this driver supports. 1 is minimum.
  * \param[in] numParams The number of parameters that the derived class supports. No longer used.
  * \param[in] maxBuffers The maximum number of NDArray buffers that the NDArrayPool for this driver is 
  *            allowed to allocate. Set this to -1 to allow an unlimited number of buffers.
  * \param[in] maxMemory The maximum amount of memory that the NDArrayPool for this driver is 
  *            allowed to allocate. Set this to -1 to allow an unlimited amount of memory.
  * \param[in] interfaceMask Bit mask defining the asyn interfaces that this driver supports.
  * \param[in] interruptMask Bit mask definining the asyn interfaces that can generate interrupts (callbacks)
  * \param[in] asynFlags Flags when creating the asyn port driver; includes ASYN_CANBLOCK and ASYN_MULTIDEVICE.
  * \param[in] autoConnect The autoConnect flag for the asyn port driver.
  * \param[in] priority The thread priority for the asyn port driver thread if ASYN_CANBLOCK is set in asynFlags.
  * \param[in] stackSize The stack size for the asyn port driver thread if ASYN_CANBLOCK is set in asynFlags.
  * \param[in] maxThreads The maximum number of threads this plugin is allowed to use.
  */
NDPluginDriver::NDPluginDriver(const char *portName, int queueSize, int blockingCallbacks, 
                               const char *NDArrayPort, int NDArrayAddr, int maxAddr, int numParams,
                               int maxBuffers, size_t maxMemory, int interfaceMask, int interruptMask,
                               int asynFlags, int autoConnect, int priority, int stackSize, int maxThreads)

    : asynNDArrayDriver(portName, maxAddr, 0, maxBuffers, maxMemory,
          interfaceMask | asynInt32Mask | asynFloat64Mask | asynOctetMask | asynInt32ArrayMask | asynDrvUserMask,
          interruptMask | asynInt32Mask | asynFloat64Mask | asynOctetMask | asynInt32ArrayMask,
          asynFlags, autoConnect, priority, stackSize),
    pluginStarted_(false),
    firstOutputArray_(true),
    pToThreadMsgQ_(NULL),
    pFromThreadMsgQ_(NULL),
    prevUniqueId_(-1000),
    sortingThreadId_(0),
    pInputArray_(0)    
{
    asynUser *pasynUser;
    //static const char *functionName = "NDPluginDriver";
    
    /* We use the same stack size for our callback thread as for the port thread */
    if (stackSize <= 0) stackSize = epicsThreadGetStackSize(epicsThreadStackMedium);
    threadStackSize_ = stackSize;
    
    lock();
    
    /* Initialize some members to 0 */
    memset(&this->lastProcessTime_, 0, sizeof(this->lastProcessTime_));
    memset(&this->dimsPrev_, 0, sizeof(this->dimsPrev_));
    this->pasynGenericPointer_ = NULL;
    this->asynGenericPointerPvt_ = NULL;
    this->asynGenericPointerInterruptPvt_ = NULL;
    this->connectedToArrayPort_ = false;
    
    if (maxThreads < 1) maxThreads = 1;
    
    /* Create asynUser for communicating with NDArray port */
    pasynUser = pasynManager->createAsynUser(0, 0);
    pasynUser->userPvt = this;
    this->pasynUserGenericPointer_ = pasynUser;
    this->pasynUserGenericPointer_->reason = NDArrayData;

    createParam(NDPluginDriverArrayPortString,         asynParamOctet, &NDPluginDriverArrayPort);
    createParam(NDPluginDriverArrayAddrString,         asynParamInt32, &NDPluginDriverArrayAddr);
    createParam(NDPluginDriverPluginTypeString,        asynParamOctet, &NDPluginDriverPluginType);
    createParam(NDPluginDriverDroppedArraysString,     asynParamInt32, &NDPluginDriverDroppedArrays);
    createParam(NDPluginDriverQueueSizeString,         asynParamInt32, &NDPluginDriverQueueSize);
    createParam(NDPluginDriverQueueFreeString,         asynParamInt32, &NDPluginDriverQueueFree);
    createParam(NDPluginDriverMaxThreadsString,        asynParamInt32, &NDPluginDriverMaxThreads);
    createParam(NDPluginDriverNumThreadsString,        asynParamInt32, &NDPluginDriverNumThreads);
    createParam(NDPluginDriverSortModeString,          asynParamInt32, &NDPluginDriverSortMode);
    createParam(NDPluginDriverSortTimeString,          asynParamFloat64, &NDPluginDriverSortTime);
    createParam(NDPluginDriverSortSizeString,          asynParamInt32, &NDPluginDriverSortSize);
    createParam(NDPluginDriverSortFreeString,          asynParamInt32, &NDPluginDriverSortFree);
    createParam(NDPluginDriverDisorderedArraysString,  asynParamInt32, &NDPluginDriverDisorderedArrays);
    createParam(NDPluginDriverDroppedOutputArraysString,  asynParamInt32, &NDPluginDriverDroppedOutputArrays);
    createParam(NDPluginDriverEnableCallbacksString,   asynParamInt32, &NDPluginDriverEnableCallbacks);
    createParam(NDPluginDriverBlockingCallbacksString, asynParamInt32, &NDPluginDriverBlockingCallbacks);
    createParam(NDPluginDriverProcessPluginString,     asynParamInt32, &NDPluginDriverProcessPlugin);
    createParam(NDPluginDriverExecutionTimeString,     asynParamFloat64, &NDPluginDriverExecutionTime);
    createParam(NDPluginDriverMinCallbackTimeString,   asynParamFloat64, &NDPluginDriverMinCallbackTime);

    /* Here we set the values of read-only parameters and of read/write parameters that cannot
     * or should not get their values from the database.  Note that values set here will override
     * those in the database for output records because if asyn device support reads a value from 
     * the driver with no error during initialization then it sets the output record to that value.  
     * If a value is not set here then the read request will return an error (uninitialized).
     * Values set here will be overridden by values from save/restore if they exist. */
    setStringParam (NDPluginDriverArrayPort, NDArrayPort);
    setIntegerParam(NDPluginDriverArrayAddr, NDArrayAddr);
    setIntegerParam(NDPluginDriverDroppedArrays, 0);
    setIntegerParam(NDPluginDriverDroppedOutputArrays, 0);
    setIntegerParam(NDPluginDriverQueueSize, queueSize);
    setIntegerParam(NDPluginDriverMaxThreads, maxThreads);
    setIntegerParam(NDPluginDriverNumThreads, maxThreads);
    setIntegerParam(NDPluginDriverBlockingCallbacks, blockingCallbacks);
    
    /* Create the callback threads, unless blocking callbacks are disabled with
     * the blockingCallbacks argument here. Even then, if they are enabled
     * subsequently, we will create the threads then. */
    if (!blockingCallbacks) {
        createCallbackThreads();
    }
    
    unlock();
}

NDPluginDriver::~NDPluginDriver()
{
  // Most methods in NDPluginDriver expect to be called with the asynPortDriver mutex locked.
  // The destructor does not, the mutex should be unlocked before calling the destructor.
  // We lock the mutex because deleteCallbackThreads expects it to be held, but then
  // unlocked it because the mutex is deleted in the asynPortDriver destructor and the
  // mutex must be unlocked before deleting it.
  this->lock();
  deleteCallbackThreads();
  this->unlock();
}

/** Method that is normally called at the beginning of the processCallbacks
  * method in derived classes.
  * \param[in] pArray  The NDArray from the callback.
  *
  * This method takes care of some bookkeeping for callbacks, updating parameters
  * from data in the class and in the NDArray.  It does asynInt32Array callbacks
  * for the dimensions array if the dimensions of the NDArray data have changed. */ 
    void NDPluginDriver::processCallbacks(NDArray *pArray)
{
    int arrayCounter;
    int i, dimsChanged;
    int size;
    NDAttribute *pAttribute;
    int colorMode=NDColorModeMono, bayerPattern=NDBayerRGGB;

    pAttribute = pArray->pAttributeList->find("ColorMode");
    if (pAttribute) pAttribute->getValue(NDAttrInt32, &colorMode);
    pAttribute = pArray->pAttributeList->find("BayerPattern");
    if (pAttribute) pAttribute->getValue(NDAttrInt32, &bayerPattern);
    
    getIntegerParam(NDArrayCounter, &arrayCounter);
    arrayCounter++;
    setIntegerParam(NDArrayCounter, arrayCounter);
    setIntegerParam(NDNDimensions, pArray->ndims);
    setIntegerParam(NDDataType, pArray->dataType);
    setIntegerParam(NDColorMode, colorMode);
    setIntegerParam(NDBayerPattern, bayerPattern);
    setIntegerParam(NDUniqueId, pArray->uniqueId);
    setTimeStamp(&pArray->epicsTS);
    setDoubleParam(NDTimeStamp, pArray->timeStamp);
    setIntegerParam(NDEpicsTSSec, pArray->epicsTS.secPastEpoch);
    setIntegerParam(NDEpicsTSNsec, pArray->epicsTS.nsec);
    /* See if the array dimensions have changed.  If so then do callbacks on them. */
    for (i=0, dimsChanged=0; i<ND_ARRAY_MAX_DIMS; i++) {
        size = (int)pArray->dims[i].size;
        if (i >= pArray->ndims) size = 0;
        if (size != this->dimsPrev_[i]) {
            this->dimsPrev_[i] = size;
            dimsChanged = 1;
        }
    }
    if (dimsChanged) {
        doCallbacksInt32Array(this->dimsPrev_, ND_ARRAY_MAX_DIMS, NDDimensions, 0);
    }
    // Save a pointer to the input array for use by ProcessPlugin
    if (pInputArray_) pInputArray_->release();
    pArray->reserve();
    pInputArray_ = pArray;
}

extern "C" {static void driverCallback(void *drvPvt, asynUser *pasynUser, void *genericPointer)
{
    NDPluginDriver *pNDPluginDriver = (NDPluginDriver *)drvPvt;
    pNDPluginDriver->driverCallback(pasynUser, genericPointer);
}}

/** Method that is called from the driver with a new NDArray.
  * It calls the processCallbacks function, which typically is implemented in the
  * derived class.
  * It can either do the callbacks directly (if NDPluginDriverBlockingCallbacks=1) or by queueing
  * the arrays to be processed by a background task (if NDPluginDriverBlockingCallbacks=0).
  * In the latter case arrays can be dropped if the queue is full.  This method should really
  * be private, but it must be called from a C-linkage callback function, so it must be public.
  * \param[in] pasynUser  The pasynUser from the asyn client.
  * \param[in] genericPointer The pointer to the NDArray */ 
void NDPluginDriver::driverCallback(asynUser *pasynUser, void *genericPointer)
{
     
    NDArray *pArray = (NDArray *)genericPointer;
    epicsTimeStamp tNow, tEnd;
    double minCallbackTime, deltaTime;
    int status=0;
    int blockingCallbacks;
    int droppedArrays, queueSize, queueFree;
    bool ignoreQueueFull = false;
    static const char *functionName = "driverCallback";

    this->lock();

    status |= getDoubleParam(NDPluginDriverMinCallbackTime, &minCallbackTime);
    status |= getIntegerParam(NDPluginDriverBlockingCallbacks, &blockingCallbacks);
    status |= getIntegerParam(NDPluginDriverQueueSize, &queueSize);
    
    epicsTimeGetCurrent(&tNow);
    deltaTime = epicsTimeDiffInSeconds(&tNow, &this->lastProcessTime_);

    if ((minCallbackTime == 0.) || (deltaTime > minCallbackTime)) {
        if (pasynUser->auxStatus == asynOverflow) ignoreQueueFull = true;
        pasynUser->auxStatus = asynSuccess;
        
        /* Time to process the next array */
        
        /* The callbacks can operate in 2 modes: blocking or non-blocking.
         * If blocking we call processCallbacks directly, executing them
         * in the detector callback thread.
         * If non-blocking we put the array on the queue and it executes
         * in our background thread. */
        /* Update the time we last posted an array */
        epicsTimeGetCurrent(&tNow);
        memcpy(&this->lastProcessTime_, &tNow, sizeof(tNow));
        if (blockingCallbacks) {
            processCallbacks(pArray);
            epicsTimeGetCurrent(&tEnd);
            setDoubleParam(NDPluginDriverExecutionTime, epicsTimeDiffInSeconds(&tEnd, &tNow)*1e3);
        } else {
            /* Increase the reference count again on this array
             * It will be released in the background task when processing is done */
            pArray->reserve();
            /* Try to put this array on the message queue.  If there is no room then return
             * immediately. */
            ToThreadMessage_t msg = {ToThreadMessageData, pArray};
            status = pToThreadMsgQ_->trySend(&msg, sizeof(msg));
            queueFree = queueSize - pToThreadMsgQ_->pending();
            setIntegerParam(NDPluginDriverQueueFree, queueFree);
            if (status) {
                pasynUser->auxStatus = asynOverflow;
                if (!ignoreQueueFull) {
                    status |= getIntegerParam(NDPluginDriverDroppedArrays, &droppedArrays);
                    asynPrint(pasynUser, ASYN_TRACE_FLOW, 
                        "%s::%s message queue full, dropped array uniqueId=%d\n",
                        driverName, functionName, pArray->uniqueId);
                    droppedArrays++;
                    status |= setIntegerParam(NDPluginDriverDroppedArrays, droppedArrays);
                }
                /* This buffer needs to be released */
                pArray->release();
            }
        }
    }
    callParamCallbacks();
    this->unlock();
}

/** Method runs as a separate thread, waiting for NDArrays to arrive in a message queue
  * and processing them.
  * This thread is used when NDPluginDriverBlockingCallbacks=0.
  * This method should really be private, but it must be called from a 
  * C-linkage callback function, so it must be public. */ 
void NDPluginDriver::processTask()
{
    /* This thread processes a new array when it arrives */
    int queueSize, queueFree;
    epicsTimeStamp tStart, tEnd;
    int numBytes;
    int status;
    NDArray *pArray=0;
    ToThreadMessage_t toMsg;
    FromThreadMessage_t fromMsg = {FromThreadMessageEnter, epicsThreadGetIdSelf()};
    static const char *functionName = "processTask";

    // Send event indicating that the thread has started. Must do this before taking lock.
    status = pFromThreadMsgQ_->send(&fromMsg, sizeof(fromMsg));
    if (status) {
        asynPrint(pasynUserSelf, ASYN_TRACE_ERROR, 
            "%s::%s error sending enter message thread %s\n",
            driverName, functionName, epicsThreadGetNameSelf());
    }
    this->lock();
    /* Loop forever */
    while (1) {

        /* Wait for an array to arrive from the queue. Release the lock while  waiting. */
        this->unlock();   
        numBytes = pToThreadMsgQ_->receive(&toMsg, sizeof(toMsg));
        if (numBytes != sizeof(toMsg)) {
            asynPrint(pasynUserSelf, ASYN_TRACE_ERROR, 
                "%s::%s error reading message queue, expected size=%d, actual=%d\n",
                driverName, functionName, (int)sizeof(toMsg), numBytes);
        }
        switch (toMsg.messageType) {
            case ToThreadMessageExit:
                asynPrint(pasynUserSelf, ASYN_TRACE_FLOW, 
                    "%s::%s received exit message, thread=%s\n", 
                    driverName, functionName, epicsThreadGetNameSelf());
                fromMsg.messageType = FromThreadMessageExit;
                pFromThreadMsgQ_->send(&fromMsg, sizeof(fromMsg));
                return; // shutdown thread if special message
                break;
            case ToThreadMessageData:
                pArray = toMsg.pArray;
                break;
            default:
                asynPrint(pasynUserSelf, ASYN_TRACE_ERROR, 
                    "%s::%s unknown message type = %d\n",
                    driverName, functionName, toMsg.messageType);
        }
        
        // Note: the lock must not be taken until after the thread exit logic above    
        this->lock();
        epicsTimeGetCurrent(&tStart);
        getIntegerParam(NDPluginDriverQueueSize, &queueSize);
        queueFree = queueSize - pToThreadMsgQ_->pending();
        setIntegerParam(NDPluginDriverQueueFree, queueFree);

        /* Call the function that does the business of this callback.
         * This function should release the lock during time-consuming operations,
         * but of course it must not access any class data when the lock is released. */
        processCallbacks(pArray); 
        
        /* We are done with this array buffer */
        pArray->release();
        epicsTimeGetCurrent(&tEnd);
        setDoubleParam(NDPluginDriverExecutionTime, epicsTimeDiffInSeconds(&tEnd, &tStart)*1e3);
        callParamCallbacks();
    }
}

/** Register or unregister to receive asynGenericPointer (NDArray) callbacks from the driver.
  * Note: this function must be called with the lock released, otherwise a deadlock can occur
  * in the call to cancelInterruptUser.
  * \param[in] enableCallbacks 1 to enable callbacks, 0 to disable callbacks */ 
asynStatus NDPluginDriver::setArrayInterrupt(int enableCallbacks)
{
    asynStatus status = asynSuccess;
    static const char *functionName = "setArrayInterrupt";
    
    if (enableCallbacks && connectedToArrayPort_ && !this->asynGenericPointerInterruptPvt_) {
        status = this->pasynGenericPointer_->registerInterruptUser(
                    this->asynGenericPointerPvt_, this->pasynUserGenericPointer_,
                    ::driverCallback, this, &this->asynGenericPointerInterruptPvt_);
        if (status != asynSuccess) {
            asynPrint(this->pasynUserSelf, ASYN_TRACE_ERROR,
                "%s::%s ERROR: Can't register for interrupt callbacks on detector port: %s\n",
                driverName, functionName, this->pasynUserGenericPointer_->errorMessage);
            return(status);
        }
    } 
    if (!enableCallbacks && connectedToArrayPort_ && this->asynGenericPointerInterruptPvt_) {
        status = this->pasynGenericPointer_->cancelInterruptUser(this->asynGenericPointerPvt_, 
                        this->pasynUserGenericPointer_, this->asynGenericPointerInterruptPvt_);
        this->asynGenericPointerInterruptPvt_ = NULL;
        if (status != asynSuccess) {
            asynPrint(this->pasynUserSelf, ASYN_TRACE_ERROR,
                "%s::%s ERROR: Can't unregister for interrupt callbacks on detector port: %s\n",
                driverName, functionName, this->pasynUserGenericPointer_->errorMessage);
            return(status);
        }
    }
    return(asynSuccess);
}

/** Connect this plugin to an NDArray port driver; disconnect from any existing driver first, register
  * for callbacks if enabled. */
asynStatus NDPluginDriver::connectToArrayPort(void)
{
    asynStatus status;
    asynInterface *pasynInterface;
    int enableCallbacks;
    char arrayPort[20];
    int arrayAddr;
    static const char *functionName = "connectToArrayPort";

    getStringParam(NDPluginDriverArrayPort, sizeof(arrayPort), arrayPort);
    getIntegerParam(NDPluginDriverArrayAddr, &arrayAddr);
    getIntegerParam(NDPluginDriverEnableCallbacks, &enableCallbacks);

    /* If we are currently connected to an array port cancel interrupt request */    
    if (this->connectedToArrayPort_) {
        status = setArrayInterrupt(0);
    }
    
    /* Disconnect the array port from our asynUser.  Ignore error if there is no device
     * currently connected. */
    pasynManager->disconnect(this->pasynUserGenericPointer_);
    this->connectedToArrayPort_ = false;

    /* Connect to the array port driver */
    status = pasynManager->connectDevice(this->pasynUserGenericPointer_, arrayPort, arrayAddr);
    if (status != asynSuccess) {
        asynPrint(this->pasynUserSelf, ASYN_TRACE_ERROR,
                  "%s::%s Error calling pasynManager->connectDevice to array port %s address %d, status=%d, error=%s\n",
                  driverName, functionName, arrayPort, arrayAddr, status, this->pasynUserGenericPointer_->errorMessage);
        return (status);
    }

    /* Find the asynGenericPointer interface in that driver */
    pasynInterface = pasynManager->findInterface(this->pasynUserGenericPointer_, asynGenericPointerType, 1);
    if (!pasynInterface) {
        asynPrint(this->pasynUserSelf, ASYN_TRACE_ERROR,
                  "%s::connectToPort ERROR: Can't find asynGenericPointer interface on array port %s address %d\n",
                  driverName, arrayPort, arrayAddr);
        return(asynError);
    }
    pasynGenericPointer_ = (asynGenericPointer *)pasynInterface->pinterface;
    asynGenericPointerPvt_ = pasynInterface->drvPvt;
    connectedToArrayPort_ = true;

    /* Enable or disable interrupt callbacks */
    status = setArrayInterrupt(enableCallbacks);

    return(status);
}   

void NDPluginDriver::sortingTask()
{
    double sortTime;
    epicsTimeStamp now;
    int sortSize;
    double deltaTime;
    int listSize;
    std::multiset<sortedListElement>::iterator pListElement;
    static const char *functionName = "sortingTask";

    lock();
    while (1) {
        getDoubleParam(NDPluginDriverSortTime, &sortTime);
        unlock();
        epicsThreadSleep(sortTime);
        lock();
        epicsTimeGetCurrent(&now);
        getIntegerParam(NDPluginDriverSortSize, &sortSize);
        while ((listSize=(int)sortedNDArrayList_.size()) > 0) {
            bool orderOK;
            pListElement = sortedNDArrayList_.begin();
            deltaTime = epicsTimeDiffInSeconds(&now, &pListElement->insertionTime_);
            asynPrint(pasynUserSelf, ASYN_TRACEIO_DRIVER, 
                "%s::%s, deltaTime=%f, list size=%d, uniqueId=%d\n", 
                driverName, functionName, deltaTime, listSize, pListElement->pArray_->uniqueId);            
            orderOK = (pListElement->pArray_->uniqueId == prevUniqueId_)   ||
                      (pListElement->pArray_->uniqueId == prevUniqueId_+1);
            if ((!firstOutputArray_ && orderOK) || (deltaTime > sortTime)) {
                this->unlock();
                doCallbacksGenericPointer(pListElement->pArray_, NDArrayData, 0);
                this->lock();
                prevUniqueId_ = pListElement->pArray_->uniqueId;
                pListElement->pArray_->release();
                sortedNDArrayList_.erase(pListElement);
                if (!firstOutputArray_ && !orderOK) {
                    int disorderedArrays;
                    getIntegerParam(NDPluginDriverDisorderedArrays, &disorderedArrays);
                    disorderedArrays++;
                    setIntegerParam(NDPluginDriverDisorderedArrays, disorderedArrays);
                }
                firstOutputArray_ = false;
            } else  {
                break;
            }
        }
        listSize=(int)sortedNDArrayList_.size();
        setIntegerParam(NDPluginDriverSortFree, sortSize-listSize);
        callParamCallbacks();
    }    
}

asynStatus NDPluginDriver::doNDArrayCallbacks(NDArray *pArray)
{
    int arrayCallbacks;
    static const char *functionName = "doNDArrayCallbacks";

    getIntegerParam(NDArrayCallbacks, &arrayCallbacks);
    if (arrayCallbacks != 1) return asynSuccess;

    int callbacksSorted;
    getIntegerParam(NDPluginDriverSortMode, &callbacksSorted);
    NDArray *pArrayOut = this->pNDArrayPool->copy(pArray, NULL, 1);
    if (NULL != pArrayOut) {
        this->getAttributes(pArrayOut->pAttributeList);
        if (this->pArrays[0]) this->pArrays[0]->release();
        this->pArrays[0] = pArrayOut;
    }
    else {
        asynPrint(this->pasynUserSelf, ASYN_TRACE_ERROR, 
            "%s::%s: Couldn't allocate output array. Further processing terminated.\n", 
            driverName, functionName);
        return asynError;
    }
    if (callbacksSorted) {
        int sortSize;
        int listSize = (int)sortedNDArrayList_.size();
        getIntegerParam(NDPluginDriverSortSize, &sortSize);
        setIntegerParam(NDPluginDriverSortFree, sortSize-listSize);
        if (listSize >= sortSize) {
            int droppedOutputArrays;
            getIntegerParam(NDPluginDriverDroppedOutputArrays, &droppedOutputArrays);
            asynPrint(pasynUserSelf, ASYN_TRACE_FLOW, 
                "%s::%s std::multilist size exceeded, dropped array uniqueId=%d\n",
                driverName, functionName, pArrayOut->uniqueId);
            droppedOutputArrays++;
            setIntegerParam(NDPluginDriverDroppedOutputArrays, droppedOutputArrays);
        } else {
            epicsTimeStamp now;
            epicsTimeGetCurrent(&now);
            pArrayOut->reserve();
            sortedListElement *pListElement = new sortedListElement(pArrayOut, now);
            sortedNDArrayList_.insert(*pListElement);
        }
    } else {
        this->unlock();
        doCallbacksGenericPointer(pArrayOut, NDArrayData, 0);
        this->lock();
        bool orderOK = (pArrayOut->uniqueId == prevUniqueId_)   ||
                       (pArrayOut->uniqueId == prevUniqueId_+1);
        if (!firstOutputArray_ && !orderOK) {
            int disorderedArrays;
            getIntegerParam(NDPluginDriverDisorderedArrays, &disorderedArrays);
            disorderedArrays++;
            setIntegerParam(NDPluginDriverDisorderedArrays, disorderedArrays);
        }
        firstOutputArray_ = false;
        prevUniqueId_ = pArrayOut->uniqueId;
    }
    return asynSuccess;
}

/** Called when asyn clients call pasynInt32->write().
  * This function performs actions for some parameters, including NDPluginDriverEnableCallbacks and
  * NDPluginDriverArrayAddr.
  * For all parameters it sets the value in the parameter library and calls any registered callbacks..
  * \param[in] pasynUser pasynUser structure that encodes the reason and address.
  * \param[in] value Value to write. */
asynStatus NDPluginDriver::writeInt32(asynUser *pasynUser, epicsInt32 value)
{
    int function = pasynUser->reason;
    int addr=0;
    asynStatus status = asynSuccess;
    static const char* functionName = "writeInt32";

    /* If this parameter belongs to a base class call its method */
    if (function < FIRST_NDPLUGIN_PARAM) {
        return asynNDArrayDriver::writeInt32(pasynUser, value);
    }

    status = getAddress(pasynUser, &addr); 
    if (status != asynSuccess) goto done;

    /* Set the parameter in the parameter library. */
    status = (asynStatus) setIntegerParam(addr, function, value);
    if (status != asynSuccess) goto done;

    /* If blocking callbacks are being disabled but the callback thread has
     * not been created yet, create it here. */
    if (function == NDPluginDriverBlockingCallbacks && !value && pThreads_.size() == 0) {
         createCallbackThreads();
     }
    
    if (function == NDPluginDriverEnableCallbacks) {
        if (value) {  
            /* We need to register to be called with interrupts from the detector driver on 
             * the asynGenericPointer interface. Must do this with the lock released. */
            this->unlock();
            status = setArrayInterrupt(1);
            this->lock();
            if (status != asynSuccess) goto done;
        } else {
            this->unlock();
            status = setArrayInterrupt(0);
            this->lock();
            if (status != asynSuccess) goto done;
            // Release the input NDArray
            if (pInputArray_) {
                pInputArray_->release();
                pInputArray_ = 0;
            }
        }

    } else if (function == NDPluginDriverArrayAddr) {
        this->unlock();
        status = connectToArrayPort();
        this->lock();
        if (status != asynSuccess) goto done;

    } else if ((function == NDPluginDriverQueueSize) ||
               (function == NDPluginDriverNumThreads)) {
        deleteCallbackThreads();
        createCallbackThreads();

    } else if ((function == NDPluginDriverSortMode) && 
               (value == 1)) {
        status = createSortingThread();

    } else if (function == NDPluginDriverProcessPlugin) {
        if (pInputArray_) {
            driverCallback(pasynUserSelf, pInputArray_);
        } else {
            asynPrint(pasynUser, ASYN_TRACE_ERROR, 
                "%s::%s cannot do ProcessPlugin, no input array cached\n", 
                driverName, functionName);
            status = asynError;
            goto done;
        }
    }
    
    done:
    /* Do callbacks so higher layers see any changes */
    callParamCallbacks(addr);
    
    if (status) 
        asynPrint(pasynUser, ASYN_TRACE_ERROR, 
              "%s::%s ERROR, status=%d, function=%d, value=%d, connectedToArrayPort_=%d\n", 
              driverName, functionName, status, function, value, this->connectedToArrayPort_);
    else        
        asynPrint(pasynUser, ASYN_TRACEIO_DRIVER, 
              "%s::%s function=%d, value=%d, connectedToArrayPort_=%d\n", 
              driverName, functionName, function, value, connectedToArrayPort_);
    return status;
}


/** Called when asyn clients call pasynOctet->write().
  * This function performs actions for some parameters, including NDPluginDriverArrayPort.
  * For all parameters it sets the value in the parameter library and calls any registered callbacks..
  * \param[in] pasynUser pasynUser structure that encodes the reason and address.
  * \param[in] value Address of the string to write.
  * \param[in] nChars Number of characters to write.
  * \param[out] nActual Number of characters actually written. */
asynStatus NDPluginDriver::writeOctet(asynUser *pasynUser, const char *value, 
                                    size_t nChars, size_t *nActual)
{
    int addr=0;
    int function = pasynUser->reason;
    asynStatus status = asynSuccess;
    static const char *functionName = "writeOctet";

    status = getAddress(pasynUser, &addr); if (status != asynSuccess) return(status);
    /* Set the parameter in the parameter library. */
    status = (asynStatus)setStringParam(addr, function, (char *)value);

    if (function == NDPluginDriverArrayPort) {
        this->unlock();
        connectToArrayPort();
        this->lock();
    } else {
        /* If this parameter belongs to a base class call its method */
        if (function < FIRST_NDPLUGIN_PARAM) 
            status = asynNDArrayDriver::writeOctet(pasynUser, value, nChars, nActual);
    }
    
     /* Do callbacks so higher layers see any changes */
    status = (asynStatus)callParamCallbacks(addr);

    if (status) 
        epicsSnprintf(pasynUser->errorMessage, pasynUser->errorMessageSize, 
                  "%s::%s: status=%d, function=%d, value=%s", 
                  driverName, functionName, status, function, value);
    else        
        asynPrint(pasynUser, ASYN_TRACEIO_DRIVER, 
              "%s::%s: function=%d, value=%s\n", 
              driverName, functionName, function, value);
    *nActual = nChars;
    return status;
}

/** Called when asyn clients call pasynInt32Array->read().
  * Returns the value of the array dimensions for the last NDArray.  
  * \param[in] pasynUser pasynUser structure that encodes the reason and address.
  * \param[in] value Pointer to the array to read.
  * \param[in] nElements Number of elements to read.
  * \param[out] nIn Number of elements actually read. */
asynStatus NDPluginDriver::readInt32Array(asynUser *pasynUser, epicsInt32 *value, 
                                         size_t nElements, size_t *nIn)
{
    int function = pasynUser->reason;
    int addr=0;
    size_t ncopy;
    asynStatus status = asynSuccess;
    static const char *functionName = "readInt32Array";

    status = getAddress(pasynUser, &addr); if (status != asynSuccess) return(status);
    if (function == NDDimensions) {
            ncopy = ND_ARRAY_MAX_DIMS;
            if (nElements < ncopy) ncopy = nElements;
            memcpy(value, this->dimsPrev_, ncopy*sizeof(*this->dimsPrev_));
            *nIn = ncopy;
    } else {
        /* If this parameter belongs to a base class call its method */
        if (function < FIRST_NDPLUGIN_PARAM) 
            status = asynNDArrayDriver::readInt32Array(pasynUser, value, nElements, nIn);
    }
    if (status) 
        epicsSnprintf(pasynUser->errorMessage, pasynUser->errorMessageSize, 
                  "%s::%s: status=%d, function=%d", 
                  driverName, functionName, status, function);
    else        
        asynPrint(pasynUser, ASYN_TRACEIO_DRIVER, 
              "%s::%s: function=%d\n", 
              driverName, functionName, function);
    return status;
}

asynStatus NDPluginDriver::start(void)
{
    assert(!this->pluginStarted_); 
    //static const char *functionName = "start";
  
    this->pluginStarted_ = true;
    // If the plugin was started with BlockingCallbacks=Yes then pThreads_.size() will be 0
    if (pThreads_.size() == 0) return asynSuccess;
  
    return startCallbackThreads();
}

asynStatus NDPluginDriver::startCallbackThreads(void)
{
    asynStatus status = asynSuccess;
    int i;
    int numBytes;
    FromThreadMessage_t fromMsg;
    static const char *functionName = "startCallbackThreads";

    for (i=0; i<numThreads_; i++) {
        pThreads_[i]->start();
  
        // Wait for the thread to say its running
        numBytes = pFromThreadMsgQ_->receive(&fromMsg, sizeof(fromMsg), 2.0);
        if (numBytes != sizeof(fromMsg)) {
            asynPrint(pasynUserSelf, ASYN_TRACE_ERROR,
                "%s::%s timeout waiting for plugin thread %d start message\n",
                driverName, functionName, i);
            status = asynError;
        } else if (fromMsg.messageType != FromThreadMessageEnter) {
            asynPrint(pasynUserSelf, ASYN_TRACE_ERROR,
                "%s::%s incorrect response from plugin thread %d = %d\n",
                driverName, functionName, i, fromMsg.messageType);
            status = asynError;
                  
        } else if (fromMsg.threadId != pThreads_[i]->getId()) {
            asynPrint(pasynUserSelf, ASYN_TRACE_ERROR,
                "%s::%s incorrect thread ID from plugin thread %d = %p should be %p\n",
                driverName, functionName, i, fromMsg.threadId, pThreads_[i]->getId());
            status = asynError;
        }        
    }
    return status;
}
    
void NDPluginDriver::run()
{
    this->processTask();
}

asynStatus NDPluginDriver::createCallbackThreads()
{
    assert(this->pThreads_.size() == 0);
    assert(this->pToThreadMsgQ_ == 0);
    assert(this->pFromThreadMsgQ_ == 0);
    
    int queueSize;
    int numThreads;
    int maxThreads;
    int enableCallbacks;
    int i;
    asynStatus status = asynSuccess;

    getIntegerParam(NDPluginDriverMaxThreads, &maxThreads);
    getIntegerParam(NDPluginDriverNumThreads, &numThreads);
    getIntegerParam(NDPluginDriverQueueSize, &queueSize);
    if (numThreads > maxThreads) numThreads = maxThreads;
    if (numThreads < 1) numThreads = 1;
    setIntegerParam(NDPluginDriverNumThreads, numThreads);
    numThreads_ = numThreads;
  
    pThreads_.resize(numThreads);

    /* Create the message queue for the input arrays */
    pToThreadMsgQ_ = new epicsMessageQueue(queueSize, sizeof(ToThreadMessage_t));
    if (!pToThreadMsgQ_) {
        /* We don't handle memory errors above, so no point in handling this. */
        cantProceed("NDPluginDriver::createCallbackThreads epicsMessageQueueCreate failure\n");
    }
    pFromThreadMsgQ_ = new epicsMessageQueue(numThreads, sizeof(FromThreadMessage_t));
    if (!pFromThreadMsgQ_) {
        /* We don't handle memory errors above, so no point in handling this. */
        cantProceed("NDPluginDriver::createCallbackThreads epicsMessageQueueCreate failure\n");
    }

    for (i=0; i<numThreads; i++) {
        /* Create the thread (but not start). */
        char taskName[256];
        epicsSnprintf(taskName, sizeof(taskName)-1, "%s_Plugin_%d", portName, i+1);
        pThreads_[i] = new epicsThread(*this, taskName, this->threadStackSize_, epicsThreadPriorityMedium);
    }

    /* If start() was already run, we also need to start the threads. */
    if (this->pluginStarted_) {
        status = startCallbackThreads();
    }
    getIntegerParam(NDPluginDriverEnableCallbacks, &enableCallbacks);
    if (enableCallbacks) this->setArrayInterrupt(1);
    return status;
}

asynStatus NDPluginDriver::deleteCallbackThreads()
{
    ToThreadMessage_t toMsg = {ToThreadMessageExit, 0};
    FromThreadMessage_t fromMsg;
    asynStatus status = asynSuccess;
    int i;
    int pending;
    int numBytes;
    static const char *functionName = "deleteCallbackThreads";
    
    //  Disable callbacks from driver so the threads will empty the message queue
    if (pToThreadMsgQ_ != 0) {
        this->unlock();
        this->setArrayInterrupt(0);
        while ((pending=pToThreadMsgQ_->pending()) > 0) {
            asynPrint(pasynUserSelf, ASYN_TRACE_FLOW, 
                "%s::%s waiting for queue to empty, pending=%d\n", 
                driverName, functionName, pending);
            epicsThreadSleep(0.05);
        }
        // Send a kill message to the threads and wait for reply.
        // Must do this with lock released else the threads may not be able to receive the message
        for (i=0; i<numThreads_; i++) {
            if (pToThreadMsgQ_->send(&toMsg, sizeof(toMsg)) != 0) {
                asynPrint(pasynUserSelf, ASYN_TRACE_ERROR,
                    "%s::%s error sending plugin thread %d exit message\n",
                    driverName, functionName, i);
                status = asynError;
            }
            asynPrint(pasynUserSelf, ASYN_TRACE_FLOW, 
                "%s::%s sent exit message %d\n", 
                driverName, functionName, i);
            numBytes = pFromThreadMsgQ_->receive(&fromMsg, sizeof(fromMsg), 2.0);
            if (numBytes != sizeof(fromMsg)) {
                asynPrint(pasynUserSelf, ASYN_TRACE_ERROR,
                    "%s::%s timeout waiting for plugin thread %d exit message\n",
                    driverName, functionName, i);
                status = asynError;
            } else if (fromMsg.messageType != FromThreadMessageExit) {
                asynPrint(pasynUserSelf, ASYN_TRACE_ERROR,
                    "%s::%s incorrect response from plugin thread %d = %d\n",
                    driverName, functionName, i, fromMsg.messageType);
                status = asynError;
            }
        }
        this->lock();
        // All threads have now been stopped.  Delete them.
        for (i=0; i<numThreads_; i++) {
            delete pThreads_[i]; // The epicsThread destructor waits for the thread to return
        }
        pThreads_.resize(0);
        delete pToThreadMsgQ_;
        pToThreadMsgQ_ = 0;
    }
    if (pFromThreadMsgQ_) {
        delete pFromThreadMsgQ_;
        pFromThreadMsgQ_ = 0;
    }
    
    return status;
}

asynStatus NDPluginDriver::createSortingThread()
{
    char taskName[256];
    static const char *functionName = "createSortingThread";
   
    // If the thread already exists return
    if (sortingThreadId_ != 0) return asynSuccess;
    
    /* Create the thread that outputs sorted NDArrays */
    epicsSnprintf(taskName, sizeof(taskName)-1, "%s_Plugin_Sort", portName);
    sortingThreadId_ = epicsThreadCreate(taskName,
                                         epicsThreadPriorityMedium,
                                         epicsThreadGetStackSize(epicsThreadStackMedium),
                                         (EPICSTHREADFUNC)sortingTaskC, this);
    if (sortingThreadId_ == 0) {
        asynPrint(pasynUserSelf, ASYN_TRACE_ERROR,
            "%s::%s error creating sortingTask thread\n", 
            driverName, functionName);
        return asynError;
    }
    return asynSuccess;
}


