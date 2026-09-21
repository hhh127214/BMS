#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cs104_slave.h"

#include "hal_thread.h"
#include "hal_time.h"

static bool running = true;

#define MAX_GI_SESSIONS 10

typedef struct {
    int state;          /* 0 - idle, 1 - GI running */
    IMasterConnection connection;
    int progress;
    int oa;             /* originator address */
} GISession;

static GISession giSessions[MAX_GI_SESSIONS];
static Semaphore gi_lock;

void
sigint_handler(int signalId)
{
    (void)signalId;
    running = false;
}

void
printCP56Time2a(CP56Time2a time)
{
    printf("%02i:%02i:%02i %02i/%02i/%04i", CP56Time2a_getHour(time), CP56Time2a_getMinute(time),
           CP56Time2a_getSecond(time), CP56Time2a_getDayOfMonth(time), CP56Time2a_getMonth(time),
           CP56Time2a_getYear(time) + 2000);
}

static void
handleGISession(GISession* session)
{
    CS101_AppLayerParameters alParams = IMasterConnection_getApplicationLayerParameters(session->connection);

    if (session->progress == 0)
    {
        /* send scaled values */
        CS101_ASDU newAsdu =
            CS101_ASDU_create(alParams, false, CS101_COT_INTERROGATED_BY_STATION, session->oa, 1, false, false);

        InformationObject io = (InformationObject)MeasuredValueScaled_create(NULL, 100, -1, IEC60870_QUALITY_GOOD);

        CS101_ASDU_addInformationObject(newAsdu, io);

        CS101_ASDU_addInformationObject(newAsdu, (InformationObject)MeasuredValueScaled_create(
                                                     (MeasuredValueScaled)io, 101, 23, IEC60870_QUALITY_GOOD));

        CS101_ASDU_addInformationObject(newAsdu, (InformationObject)MeasuredValueScaled_create(
                                                     (MeasuredValueScaled)io, 102, 2300, IEC60870_QUALITY_GOOD));

        InformationObject_destroy(io);

        IMasterConnection_sendASDU(session->connection, newAsdu);

        CS101_ASDU_destroy(newAsdu);

        session->progress = 1;
    }
    else if (session->progress == 1)
    {
        /* send single points */
        CS101_ASDU newAsdu =
            CS101_ASDU_create(alParams, false, CS101_COT_INTERROGATED_BY_STATION, session->oa, 1, false, false);

        InformationObject io =
            (InformationObject)SinglePointInformation_create(NULL, 104, true, IEC60870_QUALITY_GOOD);

        CS101_ASDU_addInformationObject(newAsdu, io);

        CS101_ASDU_addInformationObject(
            newAsdu, (InformationObject)SinglePointInformation_create((SinglePointInformation)io, 105, false,
                                                                      IEC60870_QUALITY_GOOD));

        InformationObject_destroy(io);

        IMasterConnection_sendASDU(session->connection, newAsdu);

        CS101_ASDU_destroy(newAsdu);

        session->progress = 2;
    }
    else if (session->progress == 2)
    {
        /* send more single points */
        CS101_ASDU newAsdu =
            CS101_ASDU_create(alParams, true, CS101_COT_INTERROGATED_BY_STATION, session->oa, 1, false, false);

        SinglePointInformation io = SinglePointInformation_create(NULL, 300, true, IEC60870_QUALITY_GOOD);

        CS101_ASDU_addInformationObject(newAsdu, (InformationObject)io);
        CS101_ASDU_addInformationObject(
            newAsdu, (InformationObject)SinglePointInformation_create(io, 301, false, IEC60870_QUALITY_GOOD));
        CS101_ASDU_addInformationObject(
            newAsdu, (InformationObject)SinglePointInformation_create(io, 302, true, IEC60870_QUALITY_GOOD));
        CS101_ASDU_addInformationObject(
            newAsdu, (InformationObject)SinglePointInformation_create(io, 303, false, IEC60870_QUALITY_GOOD));
        CS101_ASDU_addInformationObject(
            newAsdu, (InformationObject)SinglePointInformation_create(io, 304, true, IEC60870_QUALITY_GOOD));
        CS101_ASDU_addInformationObject(
            newAsdu, (InformationObject)SinglePointInformation_create(io, 305, false, IEC60870_QUALITY_GOOD));
        CS101_ASDU_addInformationObject(
            newAsdu, (InformationObject)SinglePointInformation_create(io, 306, true, IEC60870_QUALITY_GOOD));
        CS101_ASDU_addInformationObject(
            newAsdu, (InformationObject)SinglePointInformation_create(io, 307, false, IEC60870_QUALITY_GOOD));

        IMasterConnection_sendASDU(session->connection, newAsdu);

        CS101_ASDU_destroy(newAsdu);

        session->progress = 3;
    }
    else if (session->progress == 3)
    {
        /* send termination message */
        CS101_ASDU tempAsdu =
            CS101_ASDU_create(alParams, false, CS101_COT_INTERROGATED_BY_STATION, session->oa, 1, false, false);

        IMasterConnection_sendACT_TERM(session->connection, tempAsdu);

        CS101_ASDU_destroy(tempAsdu);

        session->state = 0;
        session->connection = NULL;
    }
}

static void
handleGeneralInterrogation()
{
    Semaphore_wait(gi_lock);

    for (int i = 0; i < MAX_GI_SESSIONS; i++)
    {
        if (giSessions[i].state == 1)
        {
            handleGISession(&giSessions[i]);
        }
    }

    Semaphore_post(gi_lock);
}

/* Callback handler to log sent or received messages (optional) */
static void
rawMessageHandler(void* parameter, IMasterConnection conneciton, uint8_t* msg, int msgSize, bool sent)
{
    if (sent)
        printf("SEND: ");
    else
        printf("RCVD: ");

    int i;
    for (i = 0; i < msgSize; i++)
    {
        printf("%02x ", msg[i]);
    }

    printf("\n");
}

static bool
clockSyncHandler(void* parameter, IMasterConnection connection, CS101_ASDU asdu, CP56Time2a newTime)
{
    printf("Process time sync command with time ");
    printCP56Time2a(newTime);
    printf("\n");

    uint64_t newSystemTimeInMs = CP56Time2a_toMsTimestamp(newTime);

    /* Set time for ACT_CON message */
    CP56Time2a_setFromMsTimestamp(newTime, Hal_getTimeInMs());

    /* TODO update system time here */

    return true;
}

static bool
interrogationHandler(void* parameter, IMasterConnection connection, CS101_ASDU asdu, uint8_t qoi)
{
    (void)parameter;

    int ca = CS101_ASDU_getCA(asdu);

    printf("Received interrogation for CASDU %i and group %i\n", ca, qoi);

    if (ca == 1) /* only handle interrogation for CA 1 */
    {
        if (qoi == 20) /* only handle station interrogation */
        {
            Semaphore_wait(gi_lock);

            /* find existing session for this connection or a free slot */
            GISession* session = NULL;

            for (int i = 0; i < MAX_GI_SESSIONS; i++)
            {
                if (giSessions[i].state == 1 && giSessions[i].connection == connection)
                {
                    /* GI already running for this connection - reject */
                    IMasterConnection_sendACT_CON(connection, asdu, true);
                    Semaphore_post(gi_lock);
                    return true;
                }
            }

            for (int i = 0; i < MAX_GI_SESSIONS; i++)
            {
                if (giSessions[i].state == 0)
                {
                    session = &giSessions[i];
                    break;
                }
            }

            if (session)
            {
                session->state = 1;
                session->connection = connection;
                session->progress = 0;
                session->oa = (uint8_t)CS101_ASDU_getOA(asdu);
                IMasterConnection_sendACT_CON(connection, asdu, false);
            }
            else
            {
                /* no free slot - reject */
                IMasterConnection_sendACT_CON(connection, asdu, true);
            }

            Semaphore_post(gi_lock);
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

static void
connectionEventHandler(void* parameter, IMasterConnection con, CS104_PeerConnectionEvent event)
{
    if (event == CS104_CON_EVENT_CONNECTION_OPENED)
    {
        printf("Connection opened (%p)\n", con);
    }
    else if (event == CS104_CON_EVENT_CONNECTION_CLOSED)
    {
        printf("Connection closed (%p)\n", con);

        Semaphore_wait(gi_lock);

        for (int i = 0; i < MAX_GI_SESSIONS; i++)
        {
            if (giSessions[i].connection == con)
            {
                giSessions[i].state = 0;
                giSessions[i].connection = NULL;
            }
        }

        Semaphore_post(gi_lock);
    }
    else if (event == CS104_CON_EVENT_ACTIVATED)
    {
        printf("Connection activated (%p)\n", con);
    }
    else if (event == CS104_CON_EVENT_DEACTIVATED)
    {
        printf("Connection deactivated (%p)\n", con);
    }
}

int
main(int argc, char** argv)
{
    /* Add Ctrl-C handler */
    signal(SIGINT, sigint_handler);

    gi_lock = Semaphore_create(1);

    memset(giSessions, 0, sizeof(giSessions));

    /* create a new slave/server instance with default connection parameters and
     * default message queue size */
    CS104_Slave slave = CS104_Slave_create(100, 100);

    CS104_Slave_setLocalAddress(slave, "0.0.0.0");

    /* Set mode to a multiple redundancy groups
     * NOTE: library has to be compiled with CONFIG_CS104_SUPPORT_SERVER_MODE_SINGLE_REDUNDANCY_GROUP enabled (=1)
     */
    CS104_Slave_setServerMode(slave, CS104_MODE_MULTIPLE_REDUNDANCY_GROUPS);

    CS104_RedundancyGroup redGroup1 = CS104_RedundancyGroup_create("red-group-1");
    CS104_RedundancyGroup_addAllowedClient(redGroup1, "192.168.2.9");

    CS104_RedundancyGroup redGroup2 = CS104_RedundancyGroup_create("red-group-2");
    CS104_RedundancyGroup_addAllowedClient(redGroup2, "192.168.2.223");
    CS104_RedundancyGroup_addAllowedClient(redGroup2, "192.168.2.222");

    CS104_RedundancyGroup redGroup3 = CS104_RedundancyGroup_create("catch-all");

    CS104_Slave_addRedundancyGroup(slave, redGroup1);
    CS104_Slave_addRedundancyGroup(slave, redGroup2);
    CS104_Slave_addRedundancyGroup(slave, redGroup3);

    /* get the connection parameters - we need them to create correct ASDUs */
    CS101_AppLayerParameters alParams = CS104_Slave_getAppLayerParameters(slave);

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
    // CS104_Slave_setRawMessageHandler(slave, rawMessageHandler, NULL);

    CS104_Slave_start(slave);

    if (CS104_Slave_isRunning(slave) == false)
    {
        printf("Starting server failed!\n");
        goto exit_program;
    }

    int16_t scaledValue = 0;

    uint64_t lastPeriodicTransmission = 0;

    while (running)
    {
        handleGeneralInterrogation();

        if (Hal_getMonotonicTimeInMs() - lastPeriodicTransmission >= 1000)
        {
            lastPeriodicTransmission = Hal_getMonotonicTimeInMs();

            CS101_ASDU newAsdu = CS101_ASDU_create(alParams, false, CS101_COT_PERIODIC, 0, 1, false, false);

            InformationObject io =
                (InformationObject)MeasuredValueScaled_create(NULL, 110, scaledValue, IEC60870_QUALITY_GOOD);

            scaledValue++;

            CS101_ASDU_addInformationObject(newAsdu, io);

            InformationObject_destroy(io);

            /* Add ASDU to slave event queue */
            CS104_Slave_enqueueASDU(slave, newAsdu);

            CS101_ASDU_destroy(newAsdu);
        }

        Thread_sleep(10);
    }

    CS104_Slave_stop(slave);

exit_program:
    CS104_Slave_destroy(slave);

    Semaphore_destroy(gi_lock);
}
