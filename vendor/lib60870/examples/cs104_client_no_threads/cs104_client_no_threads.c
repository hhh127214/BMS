#include "cs104_connection.h"
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>

static bool
asduReceived(void* parameter, int connectionIndex, CS101_ASDU asdu)
{
    (void)parameter;
    (void)connectionIndex;

    printf("RECVD ASDU type: %s(%i) elements: %i\n", TypeID_toString(CS101_ASDU_getTypeID(asdu)),
           CS101_ASDU_getTypeID(asdu), CS101_ASDU_getNumberOfElements(asdu));

    if (CS101_ASDU_getTypeID(asdu) == M_ME_TE_1)
    {
        printf("  measured scaled values with CP56Time2a timestamp:\n");

        int i;

        for (i = 0; i < CS101_ASDU_getNumberOfElements(asdu); i++)
        {
            MeasuredValueScaledWithCP56Time2a io = (MeasuredValueScaledWithCP56Time2a)CS101_ASDU_getElement(asdu, i);

            if (io)
            {
                printf("    IOA: %i value: %i\n", InformationObject_getObjectAddress((InformationObject)io),
                       MeasuredValueScaled_getValue((MeasuredValueScaled)io));

                MeasuredValueScaledWithCP56Time2a_destroy(io);
            }
        }
    }
    else if (CS101_ASDU_getTypeID(asdu) == M_SP_NA_1)
    {
        printf("  single point information:\n");

        int i;

        for (i = 0; i < CS101_ASDU_getNumberOfElements(asdu); i++)
        {
            SinglePointInformation io = (SinglePointInformation)CS101_ASDU_getElement(asdu, i);

            if (io)
            {
                printf("    IOA: %i value: %i\n", InformationObject_getObjectAddress((InformationObject)io),
                       SinglePointInformation_getValue((SinglePointInformation)io));

                SinglePointInformation_destroy(io);
            }
        }
    }
    else if (CS101_ASDU_getTypeID(asdu) == C_TS_TA_1)
    {
        printf("  test command with timestamp\n");
    }

    return true;
}

static void
connectionHandler(void* parameter, CS104_Connection connection, CS104_ConnectionEvent event)
{
    (void)parameter;
    (void)connection;

    printf("[EVENT] %d\n", event);
    if (event == CS104_CONNECTION_STARTDT_CON_RECEIVED)
    {
        printf("Sending interrogation command...\n");
        // CS104_Connection_sendInterrogationCommand(connection, CS101_COT_ACTIVATION, 1,
        //                                           (QualifierOfInterrogation)20); /* station interrogation */
    }
}

int
main(int argc, char** argv)
{
    const char* host = (argc > 1) ? argv[1] : "127.0.0.1";
    int port = (argc > 2) ? atoi(argv[2]) : -1;

    CS104_Connection con = CS104_Connection_create(host, port);

    if (!con)
    {
        fprintf(stderr, "Failed to allocate connection\n");
        return 1;
    }

    CS104_Connection_setASDUReceivedHandler(con, asduReceived, NULL);
    CS104_Connection_setConnectionHandler(con, connectionHandler, NULL);

    CS104_APCIParameters apciParams = CS104_Connection_getAPCIParameters(con);

    apciParams->t3 = 2;

    printf("APCI parameters:\n");
    printf("  t0: %i\n", apciParams->t0);
    printf("  t1: %i\n", apciParams->t1);
    printf("  t2: %i\n", apciParams->t2);
    printf("  t3: %i\n", apciParams->t3);
    printf("  k: %i\n", apciParams->k);
    printf("  w: %i\n", apciParams->w);


    if (!CS104_Connection_startThreadless(con))
    {
        fprintf(stderr, "Failed to start threadless connection\n");
        CS104_Connection_destroy(con);
        return 2;
    }

    CS104_Connection_sendStartDT(con);

    /* Simple loop: run for up to 30 seconds */
    int iterations = 0;

    while (CS104_Connection_isThreadless(con))
    {
        if (!CS104_Connection_run(con, 100))
        {
            break; /* connection closed or failed */
        }

        if (iterations == 100)
        {
            // printf("Sending interrogation command...\n");
            // CS104_Connection_sendInterrogationCommand(con, CS101_COT_ACTIVATION, 1, IEC60870_QOI_STATION);
        }

        iterations++;
    }

    CS104_Connection_stopThreadless(con);
    CS104_Connection_destroy(con);

    return 0;
}
