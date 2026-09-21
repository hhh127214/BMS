#include <stdlib.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <signal.h>

#include "cs104_slave.h"

#include "hal_thread.h"
#include "hal_time.h"

static bool running = true;

void
sigint_handler(int signalId)
{
    running = false;
}

void
printCP56Time2a(CP56Time2a time)
{
    printf("%02i:%02i:%02i %02i/%02i/%04i", CP56Time2a_getHour(time),
                             CP56Time2a_getMinute(time),
                             CP56Time2a_getSecond(time),
                             CP56Time2a_getDayOfMonth(time),
                             CP56Time2a_getMonth(time),
                             CP56Time2a_getYear(time) + 2000);
}

/* Callback handler to log sent or received messages (optional) */
static void
rawMessageHandler(void* parameter, IMasterConnection connection, uint8_t* msg, int msgSize, bool sent)
{
    (void)parameter;
    (void)connection;

    if (sent)
        printf("SEND: ");
    else
        printf("RCVD: ");

    int i;
    for (i = 0; i < msgSize; i++) {
        printf("%02x ", msg[i]);
    }

    printf("\n");
}

static bool
clockSyncHandler (void* parameter, IMasterConnection connection, CS101_ASDU asdu, CP56Time2a newTime)
{
    printf("Process time sync command with time "); printCP56Time2a(newTime); printf("\n");

    uint64_t newSystemTimeInMs = CP56Time2a_toMsTimestamp(newTime);

    /* Set time for ACT_CON message */
    CP56Time2a_setFromMsTimestamp(newTime, Hal_getTimeInMs());

    /* update system time here */

    return true;
}

static Semaphore gi_stateLock = NULL;
static int gi_state = 0; /* 0 = IDLE, 1 = IN_PROGRESS */
static IMasterConnection gi_connection = NULL;
static int gi_progress = 0;
static uint8_t gi_oa = 0;

/* this function is supposed to be called periodically and is completing the GI process that is trigger in the interrogation handler */
static void
handleGeneralInterrogation(CS104_Slave slave)
{
    Semaphore_wait(gi_stateLock);

    if ((gi_state == 1) && (gi_connection != NULL))
    {
        /* The CS101 specification only allows information objects without timestamp in GI responses */

        if (gi_progress == 0)
        {
            CS101_AppLayerParameters alParams = CS104_Slave_getAppLayerParameters(slave);

            CS101_ASDU newAsdu = CS101_ASDU_create(alParams, false, CS101_COT_INTERROGATED_BY_STATION,
                    gi_oa, 1, false, false);

            InformationObject io = (InformationObject) MeasuredValueScaled_create(NULL, 100, -1, IEC60870_QUALITY_GOOD);

            CS101_ASDU_addInformationObject(newAsdu, io);

            CS101_ASDU_addInformationObject(newAsdu, (InformationObject)
                MeasuredValueScaled_create((MeasuredValueScaled) io, 101, 23, IEC60870_QUALITY_GOOD));

            CS101_ASDU_addInformationObject(newAsdu, (InformationObject)
                MeasuredValueScaled_create((MeasuredValueScaled) io, 102, 2300, IEC60870_QUALITY_GOOD));

            InformationObject_destroy(io);

            /* bypass queues */
            if (IMasterConnection_sendASDUEx(gi_connection, newAsdu, true))
            {
                gi_progress++;
            }

            CS101_ASDU_destroy(newAsdu);
        }
        else if (gi_progress == 1)
        {
            CS101_AppLayerParameters alParams = CS104_Slave_getAppLayerParameters(slave);

            CS101_ASDU newAsdu = CS101_ASDU_create(alParams, false, CS101_COT_INTERROGATED_BY_STATION,
                        gi_oa, 1, false, false);

            InformationObject io = (InformationObject) SinglePointInformation_create(NULL, 104, true, IEC60870_QUALITY_GOOD);

            CS101_ASDU_addInformationObject(newAsdu, io);

            CS101_ASDU_addInformationObject(newAsdu, (InformationObject)
                SinglePointInformation_create((SinglePointInformation) io, 105, false, IEC60870_QUALITY_GOOD));

            InformationObject_destroy(io);

            if (IMasterConnection_sendASDUEx(gi_connection, newAsdu, true))
            {
                gi_progress++;
            }

            CS101_ASDU_destroy(newAsdu);
        }
        else if (gi_progress == 2)
        {
            CS101_AppLayerParameters alParams = CS104_Slave_getAppLayerParameters(slave);

            CS101_ASDU newAsdu = CS101_ASDU_create(alParams, true, CS101_COT_INTERROGATED_BY_STATION,
                gi_oa, 1, false, false);

            InformationObject io = NULL;

            CS101_ASDU_addInformationObject(newAsdu, io = (InformationObject) SinglePointInformation_create(NULL, 300, true, IEC60870_QUALITY_GOOD));
            CS101_ASDU_addInformationObject(newAsdu, (InformationObject) SinglePointInformation_create((SinglePointInformation) io, 301, false, IEC60870_QUALITY_GOOD));
            CS101_ASDU_addInformationObject(newAsdu, (InformationObject) SinglePointInformation_create((SinglePointInformation) io, 302, true, IEC60870_QUALITY_GOOD));
            CS101_ASDU_addInformationObject(newAsdu, (InformationObject) SinglePointInformation_create((SinglePointInformation) io, 303, false, IEC60870_QUALITY_GOOD));
            CS101_ASDU_addInformationObject(newAsdu, (InformationObject) SinglePointInformation_create((SinglePointInformation) io, 304, true, IEC60870_QUALITY_GOOD));
            CS101_ASDU_addInformationObject(newAsdu, (InformationObject) SinglePointInformation_create((SinglePointInformation) io, 305, false, IEC60870_QUALITY_GOOD));
            CS101_ASDU_addInformationObject(newAsdu, (InformationObject) SinglePointInformation_create((SinglePointInformation) io, 306, true, IEC60870_QUALITY_GOOD));
            CS101_ASDU_addInformationObject(newAsdu, (InformationObject) SinglePointInformation_create((SinglePointInformation) io, 307, false, IEC60870_QUALITY_GOOD));

            InformationObject_destroy(io);

            if (IMasterConnection_sendASDUEx(gi_connection, newAsdu, true))
            {
                gi_progress++;
            }

            CS101_ASDU_destroy(newAsdu);
        }
        else if (gi_progress == 3)
        {
            CS101_AppLayerParameters alParams = CS104_Slave_getAppLayerParameters(slave);

            CS101_ASDU newAsdu = CS101_ASDU_create(alParams, false, CS101_COT_INTERROGATED_BY_STATION,
                        gi_oa, 1, false, false);

            InformationObject io = (InformationObject) BitString32_create(NULL, 500, 0xaaaa);

            CS101_ASDU_addInformationObject(newAsdu, io);

            InformationObject_destroy(io);

            if (IMasterConnection_sendASDUEx(gi_connection, newAsdu, true))
            {
                gi_progress++;
            }

            CS101_ASDU_destroy(newAsdu);
        }
        else if (gi_progress == 4)
        {
            /* send ACT_TERM */
            CS101_ASDU newAsdu = CS101_ASDU_create(CS104_Slave_getAppLayerParameters(slave), false, CS101_COT_ACTIVATION_TERMINATION,
                            gi_oa, 1, false, false);

            InformationObject io = (InformationObject) InterrogationCommand_create(NULL, 0, 20);

            CS101_ASDU_addInformationObject(newAsdu, io);

            InformationObject_destroy(io);

            IMasterConnection_sendASDU(gi_connection, newAsdu);

            /* finished */
            gi_state = 0;
            gi_connection = NULL;
            gi_progress = 0;
        }
    }

    Semaphore_post(gi_stateLock);
}

/* Callback handler that is called when an interrogation command is received */
static bool
interrogationHandler(void* parameter, IMasterConnection connection, CS101_ASDU asdu, uint8_t qoi)
{
    (void)parameter;

    int ca = CS101_ASDU_getCA(asdu);

    printf("Received interrogation for CASDU %i and group %i\n", ca,qoi);

    if (ca == 1) /* only handle interrogation for CA 1 */
    {
        if (qoi == 20) /* only handle station interrogation */
        {
            Semaphore_wait(gi_stateLock);

            gi_state = 1;
            gi_connection = connection;
            gi_progress = 0;
            gi_oa = (uint8_t)CS101_ASDU_getOA(asdu);
            IMasterConnection_sendACT_CON(connection, asdu, false);

            Semaphore_post(gi_stateLock);
        }
        else
        {
            IMasterConnection_sendACT_CON(connection, asdu, true);
        }
    }
    else
    {
        /* send error response */
        CS101_ASDU_setCOT(asdu, CS101_COT_UNKNOWN_CA);
        CS101_ASDU_setNegative(asdu, true);
        IMasterConnection_sendASDU(connection, asdu);
    }

    return true;
}

static bool
asduHandler(void* parameter, IMasterConnection connection, CS101_ASDU asdu)
{
    if (CS101_ASDU_getTypeID(asdu) == C_SC_NA_1)
    {
        printf("received single command\n");

        if (CS101_ASDU_getCOT(asdu) == CS101_COT_ACTIVATION)
        {
            InformationObject io = CS101_ASDU_getElement(asdu, 0);

            if (io)
            {
                if (InformationObject_getObjectAddress(io) == 5000)
                {
                    SingleCommand sc = (SingleCommand)io;

                    printf("IOA: %i switch to %i\n", InformationObject_getObjectAddress(io),
                           SingleCommand_getState(sc));

                    CS101_ASDU_setCOT(asdu, CS101_COT_ACTIVATION_CON);
                }
                else
                    CS101_ASDU_setCOT(asdu, CS101_COT_UNKNOWN_IOA);

                InformationObject_destroy(io);
            }
            else
            {
                printf("ERROR: message has no valid information object\n");
                return true;
            }
        }
        else
            CS101_ASDU_setCOT(asdu, CS101_COT_UNKNOWN_COT);

        IMasterConnection_sendASDU(connection, asdu);

        return true;
    }

    return false;
}

static bool
connectionRequestHandler(void* parameter, const char* ipAddress)
{
    printf("New connection request from %s\n", ipAddress);

#if 0
    if (strcmp(ipAddress, "127.0.0.1") == 0) {
        printf("Accept connection\n");
        return true;
    }
    else {
        printf("Deny connection\n");
        return false;
    }
#else
    return true;
#endif
}

static bool connected = false;

static void
connectionEventHandler(void* parameter, IMasterConnection con, CS104_PeerConnectionEvent event)
{
    if (event == CS104_CON_EVENT_CONNECTION_OPENED)
    {
        printf("Connection opened (%p)\n", con);
        connected = true;
    }
    else if (event == CS104_CON_EVENT_CONNECTION_CLOSED)
    {
        Semaphore_wait(gi_stateLock);
        gi_state = 0;
        gi_connection = NULL;
        gi_progress = 0;
        Semaphore_post(gi_stateLock);
        printf("Connection closed (%p)\n", con);
    }
    else if (event == CS104_CON_EVENT_ACTIVATED)
    {
        printf("Connection activated (%p)\n", con);
    }
    else if (event == CS104_CON_EVENT_DEACTIVATED)
    {
        Semaphore_wait(gi_stateLock);
        gi_state = 0;
        gi_connection = NULL;
        gi_progress = 0;
        Semaphore_post(gi_stateLock);
        printf("Connection deactivated (%p)\n", con);
    }
}

int
main(int argc, char** argv)
{
    /* Add Ctrl-C handler */
    signal(SIGINT, sigint_handler);

    /* create a new slave/server instance with default connection parameters and
     * default message queue size */
    CS104_Slave slave = CS104_Slave_create(10, 10);

    CS104_Slave_setLocalAddress(slave, "0.0.0.0");

    /* Set mode to a single redundancy group
     * NOTE: library has to be compiled with CONFIG_CS104_SUPPORT_SERVER_MODE_SINGLE_REDUNDANCY_GROUP enabled (=1)
     */
    CS104_Slave_setServerMode(slave, CS104_MODE_SINGLE_REDUNDANCY_GROUP);

    /* get the connection parameters - we need them to create correct ASDUs -
     * you can also modify the parameters here when default parameters are not to be used */
    CS101_AppLayerParameters alParams = CS104_Slave_getAppLayerParameters(slave);

    /* when you have to tweak the APCI parameters (t0-t3, k, w) you can access them here */
    CS104_APCIParameters apciParams = CS104_Slave_getConnectionParameters(slave);

    printf("APCI parameters:\n");
    printf("  t0: %i\n", apciParams->t0);
    printf("  t1: %i\n", apciParams->t1);
    printf("  t2: %i\n", apciParams->t2);
    printf("  t3: %i\n", apciParams->t3);
    printf("  k: %i\n", apciParams->k);
    printf("  w: %i\n", apciParams->w);

    /* set the callback handler for the clock synchronization command */
    CS104_Slave_setClockSyncHandler(slave, clockSyncHandler, NULL);

    /* set the callback handler for the interrogation command */
    CS104_Slave_setInterrogationHandler(slave, interrogationHandler, NULL);

    /* set handler for other message types */
    CS104_Slave_setASDUHandler(slave, asduHandler, NULL);

    /* set handler to handle connection requests (optional) */
    CS104_Slave_setConnectionRequestHandler(slave, connectionRequestHandler, NULL);

    /* set handler to track connection events (optional) */
    CS104_Slave_setConnectionEventHandler(slave, connectionEventHandler, NULL);

    /* uncomment to log messages */
    //CS104_Slave_setRawMessageHandler(slave, rawMessageHandler, NULL);

    gi_stateLock = Semaphore_create(1);

    CS104_Slave_start(slave);

    if (CS104_Slave_isRunning(slave) == false) {
        printf("Starting server failed!\n");
        goto exit_program;
    }

    int16_t scaledValue = 0;
    uint64_t lastCyclicSent = 0;

    while (running)
    {
        handleGeneralInterrogation(slave);

        uint64_t currentTime = Hal_getMonotonicTimeInMs();

        if (currentTime >= lastCyclicSent + 2000)
        {
            lastCyclicSent = currentTime;

            CS101_ASDU newAsdu = CS101_ASDU_create(alParams, false, CS101_COT_PERIODIC, 0, 1, false, false);

            InformationObject io = (InformationObject) MeasuredValueScaled_create(NULL, 110, scaledValue, IEC60870_QUALITY_GOOD);

            scaledValue++;

            CS101_ASDU_addInformationObject(newAsdu, io);

            InformationObject_destroy(io);

            /* Add ASDU to slave event queue */
            CS104_Slave_enqueueASDU(slave, newAsdu);

            CS101_ASDU_destroy(newAsdu);
        }

        Thread_sleep(10);
    }

    Thread_sleep(1000);
    printf("Stopping server\n");
    CS104_Slave_stop(slave);

exit_program:
    CS104_Slave_destroy(slave);

    Semaphore_destroy(gi_stateLock);

    Thread_sleep(500);
}
