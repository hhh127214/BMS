#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cs104_slave.h"

#include "hal_thread.h"
#include "hal_time.h"

static bool running = true;

static int gi_state = 0; /* 0 - no GI running, 1 - GI is running */
static IMasterConnection gi_connection = NULL;
static int gi_progress = 0;
static int gi_oa = 0; /* originator address */
static Semaphore gi_lock;

static CS101_AppLayerParameters appLayerParameters;

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
           CP56Time2a_getSecond(time), CP56Time2a_getDayOfMonth(time), CP56Time2a_getMonth(time) + 1,
           CP56Time2a_getYear(time) + 2000);
}

static void
handleGeneralInterrogation()
{
    Semaphore_wait(gi_lock);

    if (gi_state == 1)
    {
        CS101_AppLayerParameters alParams = IMasterConnection_getApplicationLayerParameters(gi_connection);

        if (gi_progress == 0)
        {
            /* send scaled values */
            CS101_ASDU newAsdu =
                CS101_ASDU_create(alParams, false, CS101_COT_INTERROGATED_BY_STATION, gi_oa, 1, false, false);

            InformationObject io = (InformationObject)MeasuredValueScaled_create(NULL, 100, -1, IEC60870_QUALITY_GOOD);

            CS101_ASDU_addInformationObject(newAsdu, io);

            CS101_ASDU_addInformationObject(newAsdu, (InformationObject)MeasuredValueScaled_create(
                                                         (MeasuredValueScaled)io, 101, 23, IEC60870_QUALITY_GOOD));

            CS101_ASDU_addInformationObject(newAsdu, (InformationObject)MeasuredValueScaled_create(
                                                         (MeasuredValueScaled)io, 102, 2300, IEC60870_QUALITY_GOOD));

            InformationObject_destroy(io);

            IMasterConnection_sendASDU(gi_connection, newAsdu);

            CS101_ASDU_destroy(newAsdu);

            gi_progress = 1;
        }
        else if (gi_progress == 1)
        {
            /* send single points */
            CS101_ASDU newAsdu =
                CS101_ASDU_create(alParams, false, CS101_COT_INTERROGATED_BY_STATION, gi_oa, 1, false, false);

            InformationObject io =
                (InformationObject)SinglePointInformation_create(NULL, 104, true, IEC60870_QUALITY_GOOD);

            CS101_ASDU_addInformationObject(newAsdu, io);

            CS101_ASDU_addInformationObject(
                newAsdu, (InformationObject)SinglePointInformation_create((SinglePointInformation)io, 105, false,
                                                                          IEC60870_QUALITY_GOOD));

            InformationObject_destroy(io);

            IMasterConnection_sendASDU(gi_connection, newAsdu);

            CS101_ASDU_destroy(newAsdu);

            gi_progress = 2;
        }
        else if (gi_progress == 2)
        {
            /* send more single points */
            CS101_ASDU newAsdu =
                CS101_ASDU_create(alParams, true, CS101_COT_INTERROGATED_BY_STATION, gi_oa, 1, false, false);

            CS101_ASDU_addInformationObject(
                newAsdu, (InformationObject)SinglePointInformation_create(NULL, 300, true, IEC60870_QUALITY_GOOD));
            CS101_ASDU_addInformationObject(
                newAsdu, (InformationObject)SinglePointInformation_create(NULL, 301, false, IEC60870_QUALITY_GOOD));
            CS101_ASDU_addInformationObject(
                newAsdu, (InformationObject)SinglePointInformation_create(NULL, 302, true, IEC60870_QUALITY_GOOD));
            CS101_ASDU_addInformationObject(
                newAsdu, (InformationObject)SinglePointInformation_create(NULL, 303, false, IEC60870_QUALITY_GOOD));
            CS101_ASDU_addInformationObject(
                newAsdu, (InformationObject)SinglePointInformation_create(NULL, 304, true, IEC60870_QUALITY_GOOD));
            CS101_ASDU_addInformationObject(
                newAsdu, (InformationObject)SinglePointInformation_create(NULL, 305, false, IEC60870_QUALITY_GOOD));
            CS101_ASDU_addInformationObject(
                newAsdu, (InformationObject)SinglePointInformation_create(NULL, 306, true, IEC60870_QUALITY_GOOD));
            CS101_ASDU_addInformationObject(
                newAsdu, (InformationObject)SinglePointInformation_create(NULL, 307, false, IEC60870_QUALITY_GOOD));

            IMasterConnection_sendASDU(gi_connection, newAsdu);

            CS101_ASDU_destroy(newAsdu);

            gi_progress = 3;
        }
        else if (gi_progress == 3)
        {
            /* send termination message */
            CS101_ASDU tempAsdu =
                CS101_ASDU_create(alParams, false, CS101_COT_INTERROGATED_BY_STATION, gi_oa, 1, false, false);

            IMasterConnection_sendACT_TERM(gi_connection, tempAsdu);

            CS101_ASDU_destroy(tempAsdu);

            gi_state = 0;
            gi_connection = NULL;
        }
    }

    Semaphore_post(gi_lock);
}

static bool
clockSyncHandler(void* parameter, IMasterConnection connection, CS101_ASDU asdu, CP56Time2a newTime)
{
    printf("Process time sync command with time ");
    printCP56Time2a(newTime);
    printf("\n");

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

            gi_state = 1;
            gi_connection = connection;
            gi_progress = 0;
            gi_oa = (uint8_t)CS101_ASDU_getOA(asdu);
            IMasterConnection_sendACT_CON(connection, asdu, false);

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
                printf("ERROR: ASDU contains no information object!\n");
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
    printf("New connection from %s\n", ipAddress);

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
securityEventHandler(void* parameter, TLSEventLevel eventLevel, int eventCode, const char* msg, TLSConnection con)
{
    (void)parameter;

    char peerAddrBuf[60];
    char* peerAddr = NULL;
    const char* tlsVersion = "unknown";

    if (con)
    {
        peerAddr = TLSConnection_getPeerAddress(con, peerAddrBuf);
        tlsVersion = TLSConfigVersion_toString(TLSConnection_getTLSVersion(con));
    }

    printf("[SECURITY EVENT] %s (t: %i, c: %i, version: %s remote-ip: %s)\n", msg, eventLevel, eventCode, tlsVersion,
           peerAddr);
}

int
main(int argc, char** argv)
{
    /* Add Ctrl-C handler */
    signal(SIGINT, sigint_handler);

    gi_lock = Semaphore_create(1);

    TLSConfiguration tlsConfig = TLSConfiguration_create();

    TLSConfiguration_setEventHandler(tlsConfig, securityEventHandler, NULL);

    TLSConfiguration_setMinTlsVersion(tlsConfig, TLS_VERSION_TLS_1_2);

    TLSConfiguration_setChainValidation(tlsConfig, false);
    TLSConfiguration_setAllowOnlyKnownCertificates(tlsConfig, true);

    TLSConfiguration_setOwnKeyFromFile(tlsConfig, "server_CA1_1.key", NULL);
    TLSConfiguration_setOwnCertificateFromFile(tlsConfig, "server_CA1_1.pem");
    TLSConfiguration_addCACertificateFromFile(tlsConfig, "root_CA1.pem");

    TLSConfiguration_addAllowedCertificateFromFile(tlsConfig, "client_CA1_1.pem");

    TLSConfiguration_setRenegotiationTime(tlsConfig, 2000);

    /* create a new slave/server instance */
    CS104_Slave slave = CS104_Slave_createSecure(100, 100, tlsConfig);

    CS104_Slave_setLocalAddress(slave, "0.0.0.0");

    /* get the connection parameters - we need them to create correct ASDUs */
    appLayerParameters = CS104_Slave_getAppLayerParameters(slave);

    /* set the callback handler for the clock synchronization command */
    CS104_Slave_setClockSyncHandler(slave, clockSyncHandler, NULL);

    /* set the callback handler for the interrogation command */
    CS104_Slave_setInterrogationHandler(slave, interrogationHandler, NULL);

    /* set handler for other message types */
    CS104_Slave_setASDUHandler(slave, asduHandler, NULL);

    CS104_Slave_setConnectionRequestHandler(slave, connectionRequestHandler, NULL);

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

            CS101_ASDU newAsdu = CS101_ASDU_create(appLayerParameters, false, CS101_COT_PERIODIC, 0, 1, false, false);

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

    TLSConfiguration_destroy(tlsConfig);

    Semaphore_destroy(gi_lock);
}
