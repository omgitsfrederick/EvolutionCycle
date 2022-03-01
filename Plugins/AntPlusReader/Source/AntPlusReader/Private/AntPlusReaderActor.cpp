// Fill out your copyright notice in the Description page of Project Settings.


#include "AntPlusReaderActor.h"

#include "MySaveGame.h"
#include "Kismet/GameplayStatics.h"

#include "types.h"
#include "dsi_framer_ant.hpp"
#include "dsi_thread.h"
#include "dsi_serial_generic.hpp"
#include "dsi_debug.hpp"

extern "C" {
#include "PowerDecoder.h"
}

#include <stdio.h>
#include <assert.h>
#include <string.h>
#include <time.h>

int APower;
int ACadence;

#define ENABLE_EXTENDED_MESSAGES

#define USER_BAUDRATE         (50000)  // For AT3/AP2, use 57600
#define USER_RADIOFREQ        (57) // RF Channel 57 (2457 MZ) is used for ANT+ FE Devices

#define USER_ANTCHANNEL       (0) // Slave (Controller or Display)

#define USER_NETWORK_KEY      {0xB9, 0xA5, 0x21, 0xFB, 0xBD, 0x72, 0xC3, 0x45}  //ANT+ Network Key
#define USER_NETWORK_NUM      (0)      // The network key is assigned to this network number

#define MESSAGE_TIMEOUT       (1000)

// Indexes into message recieved from ANT
#define MESSAGE_BUFFER_DATA1_INDEX ((UCHAR) 0)
#define MESSAGE_BUFFER_DATA2_INDEX ((UCHAR) 1)
#define MESSAGE_BUFFER_DATA3_INDEX ((UCHAR) 2)
#define MESSAGE_BUFFER_DATA4_INDEX ((UCHAR) 3)
#define MESSAGE_BUFFER_DATA5_INDEX ((UCHAR) 4)
#define MESSAGE_BUFFER_DATA6_INDEX ((UCHAR) 5)
#define MESSAGE_BUFFER_DATA7_INDEX ((UCHAR) 6)
#define MESSAGE_BUFFER_DATA8_INDEX ((UCHAR) 7)
#define MESSAGE_BUFFER_DATA9_INDEX ((UCHAR) 8)
#define MESSAGE_BUFFER_DATA10_INDEX ((UCHAR) 9)
#define MESSAGE_BUFFER_DATA11_INDEX ((UCHAR) 10)
#define MESSAGE_BUFFER_DATA12_INDEX ((UCHAR) 11)
#define MESSAGE_BUFFER_DATA13_INDEX ((UCHAR) 12)
#define MESSAGE_BUFFER_DATA14_INDEX ((UCHAR) 13)
#define MESSAGE_BUFFER_DATA15_INDEX ((UCHAR) 14)
#define MESSAGE_BUFFER_DATA16_INDEX ((UCHAR) 15)

// Sets default values
AAntPlusReaderActor::AAntPlusReaderActor()
{
 	// Set this actor to call Tick() every frame.  You can turn this off to improve performance if you don't need it.
	PrimaryActorTick.bCanEverTick = true;
}

// Called when the game starts or when spawned
void AAntPlusReaderActor::BeginPlay()
{
	Super::BeginPlay();

    // //Set Initial Values

    //Set pointers to null by default
    pclSerialObject = (DSISerialGeneric*)NULL;
    pclMessageObject = (DSIFramerANT*)NULL;

    //Booleans for program
    bDisplay = TRUE;
    bBroadcasting = FALSE;
    bPowerDecoderInitialized = FALSE;

    //Other various variables
    ulNewEventTime = 0;
    usPreviousEventTime = 0;
    previousRxTime = time(NULL);
    ucPowerOnlyUpdateEventCount = 0;
    dRxTimeTePs = 0;
    memset(aucTransmitBuffer, 0, ANT_STANDARD_DATA_PAYLOAD_SIZE);

    dRecordInterval = 1; //time in seconds between records generated by power decoder (Default 1)
    dTimeBase = 0; //Power Meter Timebase // 0 seconds for event based power meters (Default 0)
    dReSyncInterval = 10; //Maximum time allow for a dropout //no power recieved (Default 10)
    SetPowerMeterType(16); //(16-Power Only,17-Wheel Torque,18-CrankTorque,32-CTF,255-Unknown)
    type = -1;

    UE_LOG(LogTemp, Warning, TEXT("Ant+ Initial Values Set"));
}

// Called every frame
void AAntPlusReaderActor::Tick(float DeltaTime)
{
	Super::Tick(DeltaTime);

    AveragePower = APower;
    AverageCadence = ACadence;
}

// Quit
void AAntPlusReaderActor::Quit()
{
    UE_LOG(LogTemp, Warning, TEXT("Closing channel..."));
    bBroadcasting = FALSE;
    pclMessageObject->CloseChannel(USER_ANTCHANNEL, MESSAGE_TIMEOUT);
}

bool AAntPlusReaderActor::SetChannelID(int DevID, int DevType, int TransType, DSISerialGeneric* pclSO, DSIFramerANT* pclMO)
{
    DeviceNumber = DevID;
    DeviceType = DevType;
    TransmissionType = TransType;
    pclSerialObject = pclSO;
    pclMessageObject = pclMO;

    if (DevType == 11)
        type = 0;
    else if (DevType == 17)
        type = 1;
    else
        type = 2;

    BOOL bStatus;

    bStatus = pclMessageObject->AssignChannel(type + 1, 0, 0, MESSAGE_TIMEOUT);
    bStatus = pclMessageObject->SetChannelID(type + 1, DeviceNumber, DeviceType, TransmissionType, MESSAGE_TIMEOUT);
    bStatus = pclMessageObject->SetChannelRFFrequency(type + 1, USER_RADIOFREQ, MESSAGE_TIMEOUT);
    bStatus = pclMessageObject->SetChannelPeriod(type + 1, 8182, MESSAGE_TIMEOUT);
    bStatus = pclMessageObject->OpenChannel(type + 1, MESSAGE_TIMEOUT);

    if (bStatus)
    {
        UE_LOG(LogTemp, Warning, TEXT("Reader Set Up With Channel ID %i/%i/%i"), DeviceNumber, DeviceType, TransmissionType);
        (new FAutoDeleteAsyncTask<WaitForMessagesTask2>(pclMessageObject))->StartBackgroundTask();
        return true;
    }
    else
    {
        UE_LOG(LogTemp, Warning, TEXT("Failed to set up channel of ID %i/%i/%i"), DeviceNumber, DeviceType, TransmissionType);
        return false;
    }
}

void AAntPlusReaderActor::ProcessMessage(ANT_MESSAGE stMessage, USHORT usSize_)
{
    BOOL bStatus;
    BOOL bPrintBuffer = FALSE;
    UCHAR ucDataOffset = MESSAGE_BUFFER_DATA2_INDEX;   // For most data messages


    switch (stMessage.ucMessageID)
    {
    
    case MESG_RESPONSE_EVENT_ID: //Channel Message (Channel Response/Event)
    {
      
        switch (stMessage.aucData[1]) //Initiating Message ID
        {
        case MESG_NETWORK_KEY_ID: //Config Messages (Set Network Key)
        {
            if (stMessage.aucData[2] != RESPONSE_NO_ERROR)
            {
                UE_LOG(LogTemp, Warning, TEXT("Error configuring network key: Code 0%d"), stMessage.aucData[2]);
                break;
            }
            UE_LOG(LogTemp, Warning, TEXT("Network key set."));
            UE_LOG(LogTemp, Warning, TEXT("Assigning channel..."));
            bStatus = pclMessageObject->AssignChannel(USER_ANTCHANNEL, 0, 0, MESSAGE_TIMEOUT);
            break;
        }

        case MESG_ASSIGN_CHANNEL_ID: //Config Messages (Assign Channel)
        {
            if (stMessage.aucData[2] != RESPONSE_NO_ERROR)
            {
                UE_LOG(LogTemp, Warning, TEXT("Error assigning channel: Code 0%d"), stMessage.aucData[2]);
                break;
            }
            UE_LOG(LogTemp, Warning, TEXT("Channel assigned"));
            UE_LOG(LogTemp, Warning, TEXT("Setting Channel ID..."));
            bStatus = pclMessageObject->SetChannelID(USER_ANTCHANNEL, DeviceNumber, DeviceType, TransmissionType, MESSAGE_TIMEOUT);
            break;
        }

        case MESG_CHANNEL_ID_ID: //Config Messages (Channel ID)
        {
            if (stMessage.aucData[2] != RESPONSE_NO_ERROR)
            {
                UE_LOG(LogTemp, Warning, TEXT("Error configuring Channel ID: Code 0%d"), stMessage.aucData[2]);
                break;
            }
            UE_LOG(LogTemp, Warning, TEXT("Channel ID set"));
            UE_LOG(LogTemp, Warning, TEXT("Setting Radio Frequency..."));
            bStatus = pclMessageObject->SetChannelRFFrequency(USER_ANTCHANNEL, USER_RADIOFREQ, MESSAGE_TIMEOUT);
            break;
        }

        case MESG_CHANNEL_RADIO_FREQ_ID: //Config Messages (Channel RF Frequency)
        {
            if (stMessage.aucData[2] != RESPONSE_NO_ERROR)
            {
                UE_LOG(LogTemp, Warning, TEXT("Error configuring Radio Frequency: Code 0%d"), stMessage.aucData[2]);
                break;
            }
            UE_LOG(LogTemp, Warning, TEXT("Radio Frequency set"));
            UE_LOG(LogTemp, Warning, TEXT("Setting Channel Period..."));
            bStatus = pclMessageObject->SetChannelPeriod(USER_ANTCHANNEL, 8182, MESSAGE_TIMEOUT);
            break;
        }

        case MESG_CHANNEL_MESG_PERIOD_ID: //Config Messages (Channel Period)
        {
            if (stMessage.aucData[2] != RESPONSE_NO_ERROR)
            {
                UE_LOG(LogTemp, Warning, TEXT("Error configuring Channel Period: Code 0%d"), stMessage.aucData[2]);
                break;
            }
            UE_LOG(LogTemp, Warning, TEXT("Channel Period set"));
            UE_LOG(LogTemp, Warning, TEXT("Opening channel..."));
            bBroadcasting = TRUE;
            bStatus = pclMessageObject->OpenChannel(USER_ANTCHANNEL, MESSAGE_TIMEOUT);
            break;
        }

        case MESG_OPEN_CHANNEL_ID: //Control Messages (Open Channel)
        {
            if (stMessage.aucData[2] != RESPONSE_NO_ERROR)
            {
                UE_LOG(LogTemp, Warning, TEXT("Error opening channel: Code 0%d"), stMessage.aucData[2]);
                bBroadcasting = FALSE;
                break;
            }
            UE_LOG(LogTemp, Warning, TEXT("Channel opened"));

            // We register the power record receiver and initialize the bike power decoders after the channel has opened
            InitPowerDecoder(dRecordInterval, dTimeBase, dReSyncInterval, RecordReceiver);
            bPowerDecoderInitialized = TRUE;
            UE_LOG(LogTemp, Warning, TEXT("Power record decode library initialized"));

#if defined (ENABLE_EXTENDED_MESSAGES)
            UE_LOG(LogTemp, Warning, TEXT("Enabling extended messages..."));
            pclMessageObject->SetLibConfig(ANT_LIB_CONFIG_MESG_OUT_INC_TIME_STAMP | ANT_LIB_CONFIG_MESG_OUT_INC_DEVICE_ID, MESSAGE_TIMEOUT);
#endif
            break;
        }

        case MESG_ANTLIB_CONFIG_ID: //Config Messages (Lib Config)
        {
            if (stMessage.aucData[2] == INVALID_MESSAGE)
            {
                UE_LOG(LogTemp, Warning, TEXT("Extended messages not supported in this ANT product"));
                break;
            }
            else if (stMessage.aucData[2] != RESPONSE_NO_ERROR)
            {
                UE_LOG(LogTemp, Warning, TEXT("Error enabling extended messages: Code 0%d"), stMessage.aucData[2]);
                break;
            }
            UE_LOG(LogTemp, Warning, TEXT("Extended messages enabled"));
            break;
        }

        case MESG_UNASSIGN_CHANNEL_ID: //Config Messages (Unassign Channel)
        {
            if (stMessage.aucData[2] != RESPONSE_NO_ERROR)
            {
                UE_LOG(LogTemp, Warning, TEXT("Error unassigning channel: Code 0%d"), stMessage.aucData[2]);
                break;
            }
            UE_LOG(LogTemp, Warning, TEXT("Channel unassigned"));
            return;
            break;
        }

        case MESG_CLOSE_CHANNEL_ID: //Control Messages (Close Channel)
        {
            if (stMessage.aucData[2] == CHANNEL_IN_WRONG_STATE)
            {
                // We get here if we tried to close the channel after the search timeout (slave)
                UE_LOG(LogTemp, Warning, TEXT("Channel is already closed"));
                UE_LOG(LogTemp, Warning, TEXT("Unassigning channel..."));
                bStatus = pclMessageObject->UnAssignChannel(USER_ANTCHANNEL, MESSAGE_TIMEOUT);
                break;
            }
            else if (stMessage.aucData[2] != RESPONSE_NO_ERROR)
            {
                UE_LOG(LogTemp, Warning, TEXT("Error closing channel: Code 0%d"), stMessage.aucData[2]);
                break;
            }
            // If this message was successful, wait for EVENT_CHANNEL_CLOSED to confirm channel is closed
            break;
        }

        case MESG_REQUEST_ID: //Control Messages (Request Message)
        {
            if (stMessage.aucData[2] == INVALID_MESSAGE)
            {
                UE_LOG(LogTemp, Warning, TEXT("Requested message not supported in this ANT product"));
            }
            break;
        }

        case MESG_EVENT_ID: //Channel Event (NOT CHANNEL RESPONSE)
        {
            switch (stMessage.aucData[2]) //Event Code
            {
            case EVENT_CHANNEL_CLOSED: 
            {
                /*The channel has been successfully closed. When the Host sends a message to close a channel, it first receives a
                RESPONSE_NO_ERROR to indicate that the message was successfully received by ANT; however, EVENT_CHANNEL_CLOSED
                is the actual indication of the closure of the channel. As such, the Host must use this event message rather
                than the RESPONSE_NO_ERROR message to let a channel state machine continue.*/
                UE_LOG(LogTemp, Warning, TEXT("Channel Closed"));
                UE_LOG(LogTemp, Warning, TEXT("Unassigning channel..."));
                bStatus = pclMessageObject->UnAssignChannel(USER_ANTCHANNEL, MESSAGE_TIMEOUT);
                break;
            }
            case EVENT_TX:
            {
                /*A Broadcast message has been transmitted successfully. This event should be used to send the next message 
                for transmission to the ANT device if the node is setup as a master.*/
                // This event indicates that a message has just been
                // sent over the air. We take advantage of this event to set
                // up the data for the next message period.
                static UCHAR ucIncrement = 0;      // Increment the first byte of the buffer

                aucTransmitBuffer[0] = ucIncrement++;

                // Broadcast data will be sent over the air on
                // the next message period.
                if (bBroadcasting)
                {
                    pclMessageObject->SendBroadcastData(USER_ANTCHANNEL, aucTransmitBuffer);

                    // Echo what the data will be over the air on the next message period.
                    if (bDisplay)
                    {
                        UE_LOG(LogTemp, Warning, TEXT("Echo what the data will be over the air on the next message period."));
                        UE_LOG(LogTemp, Warning, TEXT("Tx:(%d): [%02x],[%02x],[%02x],[%02x],[%02x],[%02x],[%02x],[%02x]"),
                            USER_ANTCHANNEL,
                            aucTransmitBuffer[MESSAGE_BUFFER_DATA1_INDEX],
                            aucTransmitBuffer[MESSAGE_BUFFER_DATA2_INDEX],
                            aucTransmitBuffer[MESSAGE_BUFFER_DATA3_INDEX],
                            aucTransmitBuffer[MESSAGE_BUFFER_DATA4_INDEX],
                            aucTransmitBuffer[MESSAGE_BUFFER_DATA5_INDEX],
                            aucTransmitBuffer[MESSAGE_BUFFER_DATA6_INDEX],
                            aucTransmitBuffer[MESSAGE_BUFFER_DATA7_INDEX],
                            aucTransmitBuffer[MESSAGE_BUFFER_DATA8_INDEX]);
                    }
                    else
                    {
                        static int iIndex = 0;
                        static char ac[] = { '|', '/', '-', '\\' };
                        UE_LOG(LogTemp, Warning, TEXT("Tx: %c\r"), ac[iIndex++]); 
                        fflush(stdout);
                        iIndex &= 3;
                    }
                }
                break;

            }
            case EVENT_RX_SEARCH_TIMEOUT:
            {
                /*A receive channel has timed out on searching. The search is terminated, and the channel has been 
                automatically closed. In order to restart the search the Open Channel message must be sent again.*/
                UE_LOG(LogTemp, Warning, TEXT("Search Timeout"));
                break;
            }
            case EVENT_RX_FAIL:
            {
                /*A receive channel missed a message which it was expecting. This happens when a slave is tracking a 
                master and is expecting a message at the set message rate. */
                UE_LOG(LogTemp, Warning, TEXT("Rx Fail"));
                break;
            }
            case EVENT_TRANSFER_RX_FAILED:
            {
                /*A receive transfer has failed. This occurs when a Burst Transfer Message was incorrectly received*/
                UE_LOG(LogTemp, Warning, TEXT("Burst receive has failed"));
                break;
            }
            case EVENT_TRANSFER_TX_COMPLETED:
            {
                /*An Acknowledged Data message or a Burst Transfer sequence has been completed successfully. When transmitting 
                Acknowledged Data or Burst Transfer, there is no EVENT_TX message*/
                UE_LOG(LogTemp, Warning, TEXT("Tranfer Completed"));
                break;
            }
            case EVENT_TRANSFER_TX_FAILED:
            {
                /*An Acknowledged Data message, or a Burst Transfer Message has been initiated and the transmission failed to complete 
                successfully*/
                UE_LOG(LogTemp, Warning, TEXT("Tranfer Failed"));
                break;
            }
            case EVENT_RX_FAIL_GO_TO_SEARCH:
            {
                /*The channel has dropped to search mode after missing too many messages.*/
                UE_LOG(LogTemp, Warning, TEXT("Go to Search"));
                break;
            }
            case EVENT_CHANNEL_COLLISION:
            {
                /*Two channels have drifted into each other and overlapped in time on the device causing one channel to be blocked.*/
                UE_LOG(LogTemp, Warning, TEXT("Channel Collision"));
                break;
            }
            case EVENT_TRANSFER_TX_START:
            {
                /*Sent after a burst transfer begins, effectively on the next channel period after the burst transfer message has been 
                sent to the device. */
                UE_LOG(LogTemp, Warning, TEXT("Burst Started"));
                break;
            }
            default:
            {
                /*Wtf happened lol*/
                UE_LOG(LogTemp, Warning, TEXT("Unhandled channel event: 0x%X"), stMessage.aucData[2]);
                break;
            }

            }

            break;
        }

        default:
        {
            UE_LOG(LogTemp, Warning, TEXT("Unhandled response 0%d to message 0x%X"), stMessage.aucData[2], stMessage.aucData[1]);
            break;
        }
        }
        break;
    }

    case MESG_STARTUP_MESG_ID: //Notifications (Start-up Messsage)
    {
        UE_LOG(LogTemp, Warning, TEXT("RESET Complete, reason: "));

        UCHAR ucReason = stMessage.aucData[MESSAGE_BUFFER_DATA1_INDEX];

        if (ucReason == RESET_POR)
            UE_LOG(LogTemp, Warning, TEXT("RESET_POR"));
        if (ucReason & RESET_SUSPEND)
            UE_LOG(LogTemp, Warning, TEXT("RESET_SUSPEND "));
        if (ucReason & RESET_SYNC)
            UE_LOG(LogTemp, Warning, TEXT("RESET_SYNC "));
        if (ucReason & RESET_CMD)
            UE_LOG(LogTemp, Warning, TEXT("RESET_CMD "));
        if (ucReason & RESET_WDT)
            UE_LOG(LogTemp, Warning, TEXT("RESET_WDT "));
        if (ucReason & RESET_RST)
            UE_LOG(LogTemp, Warning, TEXT("RESET_RST "));
        UE_LOG(LogTemp, Warning, TEXT(""));

        break;
    }
    case MESG_CAPABILITIES_ID: //Requested Response Message (Capabilities)
    {
        UE_LOG(LogTemp, Warning, TEXT("CAPABILITIES:"));
        UE_LOG(LogTemp, Warning, TEXT("   Max ANT Channels: %d"), stMessage.aucData[MESSAGE_BUFFER_DATA1_INDEX]);
        UE_LOG(LogTemp, Warning, TEXT("   Max ANT Networks: %d"), stMessage.aucData[MESSAGE_BUFFER_DATA2_INDEX]);

        UCHAR ucStandardOptions = stMessage.aucData[MESSAGE_BUFFER_DATA3_INDEX];
        UCHAR ucAdvanced = stMessage.aucData[MESSAGE_BUFFER_DATA4_INDEX];
        UCHAR ucAdvanced2 = stMessage.aucData[MESSAGE_BUFFER_DATA5_INDEX];

        UE_LOG(LogTemp, Warning, TEXT("Standard Options:"));
        if (ucStandardOptions & CAPABILITIES_NO_RX_CHANNELS)
            UE_LOG(LogTemp, Warning, TEXT("CAPABILITIES_NO_RX_CHANNELS"));
        if (ucStandardOptions & CAPABILITIES_NO_TX_CHANNELS)
            UE_LOG(LogTemp, Warning, TEXT("CAPABILITIES_NO_TX_CHANNELS"));
        if (ucStandardOptions & CAPABILITIES_NO_RX_MESSAGES)
            UE_LOG(LogTemp, Warning, TEXT("CAPABILITIES_NO_RX_MESSAGES"));
        if (ucStandardOptions & CAPABILITIES_NO_TX_MESSAGES)
            UE_LOG(LogTemp, Warning, TEXT("CAPABILITIES_NO_TX_MESSAGES"));
        if (ucStandardOptions & CAPABILITIES_NO_ACKD_MESSAGES)
            UE_LOG(LogTemp, Warning, TEXT("CAPABILITIES_NO_ACKD_MESSAGES"));
        if (ucStandardOptions & CAPABILITIES_NO_BURST_TRANSFER)
            UE_LOG(LogTemp, Warning, TEXT("CAPABILITIES_NO_BURST_TRANSFER"));

        UE_LOG(LogTemp, Warning, TEXT("Advanced Options:"));
        if (ucAdvanced & CAPABILITIES_OVERUN_UNDERRUN)
            UE_LOG(LogTemp, Warning, TEXT("CAPABILITIES_OVERUN_UNDERRUN"));
        if (ucAdvanced & CAPABILITIES_NETWORK_ENABLED)
            UE_LOG(LogTemp, Warning, TEXT("CAPABILITIES_NETWORK_ENABLED"));
        if (ucAdvanced & CAPABILITIES_AP1_VERSION_2)
            UE_LOG(LogTemp, Warning, TEXT("CAPABILITIES_AP1_VERSION_2"));
        if (ucAdvanced & CAPABILITIES_SERIAL_NUMBER_ENABLED)
            UE_LOG(LogTemp, Warning, TEXT("CAPABILITIES_SERIAL_NUMBER_ENABLED"));
        if (ucAdvanced & CAPABILITIES_PER_CHANNEL_TX_POWER_ENABLED)
            UE_LOG(LogTemp, Warning, TEXT("CAPABILITIES_PER_CHANNEL_TX_POWER_ENABLED"));
        if (ucAdvanced & CAPABILITIES_LOW_PRIORITY_SEARCH_ENABLED)
            UE_LOG(LogTemp, Warning, TEXT("CAPABILITIES_LOW_PRIORITY_SEARCH_ENABLED"));
        if (ucAdvanced & CAPABILITIES_SCRIPT_ENABLED)
            UE_LOG(LogTemp, Warning, TEXT("CAPABILITIES_SCRIPT_ENABLED"));
        if (ucAdvanced & CAPABILITIES_SEARCH_LIST_ENABLED)
            UE_LOG(LogTemp, Warning, TEXT("CAPABILITIES_SEARCH_LIST_ENABLED"));

        if (usSize_ > 4)
        {
            UE_LOG(LogTemp, Warning, TEXT("Advanced 2 Options 1:"));
            if (ucAdvanced2 & CAPABILITIES_LED_ENABLED)
                UE_LOG(LogTemp, Warning, TEXT("CAPABILITIES_LED_ENABLED"));
            if (ucAdvanced2 & CAPABILITIES_EXT_MESSAGE_ENABLED)
                UE_LOG(LogTemp, Warning, TEXT("CAPABILITIES_EXT_MESSAGE_ENABLED"));
            if (ucAdvanced2 & CAPABILITIES_SCAN_MODE_ENABLED)
                UE_LOG(LogTemp, Warning, TEXT("CAPABILITIES_SCAN_MODE_ENABLED"));
            if (ucAdvanced2 & CAPABILITIES_RESERVED)
                UE_LOG(LogTemp, Warning, TEXT("CAPABILITIES_RESERVED"));
            if (ucAdvanced2 & CAPABILITIES_PROX_SEARCH_ENABLED)
                UE_LOG(LogTemp, Warning, TEXT("CAPABILITIES_PROX_SEARCH_ENABLED"));
            if (ucAdvanced2 & CAPABILITIES_EXT_ASSIGN_ENABLED)
                UE_LOG(LogTemp, Warning, TEXT("CAPABILITIES_EXT_ASSIGN_ENABLED"));
            if (ucAdvanced2 & CAPABILITIES_FS_ANTFS_ENABLED)
                UE_LOG(LogTemp, Warning, TEXT("CAPABILITIES_FREE_1"));
            if (ucAdvanced2 & CAPABILITIES_FIT1_ENABLED)
                UE_LOG(LogTemp, Warning, TEXT("CAPABILITIES_FIT1_ENABLED"));
        }
        break;
    }
    case MESG_CHANNEL_STATUS_ID: //Requested Response Message (Channel Status)
    {
        UE_LOG(LogTemp, Warning, TEXT("Got Status"));

        char astrStatus[][32] = { "STATUS_UNASSIGNED_CHANNEL",
            "STATUS_ASSIGNED_CHANNEL",
            "STATUS_SEARCHING_CHANNEL",
            "STATUS_TRACKING_CHANNEL" };

        UCHAR ucStatusByte = stMessage.aucData[MESSAGE_BUFFER_DATA2_INDEX] & STATUS_CHANNEL_STATE_MASK; // MUST MASK OFF THE RESERVED BITS
        UE_LOG(LogTemp, Warning, TEXT("STATUS: %s"), astrStatus[ucStatusByte]);
        break;
    }
    case MESG_CHANNEL_ID_ID: //Requested Response Message (Channel ID)
    {
        UE_LOG(LogTemp, Warning, TEXT("CHANNEL ID: (%d/%d/%d)"), DeviceNumber, DeviceType, TransmissionType);
        break;
    }
    case MESG_VERSION_ID: //Requested Response Message (ANT Version)
    {
        UE_LOG(LogTemp, Warning, TEXT("VERSION: %s"), (char*)&stMessage.aucData[MESSAGE_BUFFER_DATA1_INDEX]);
        break;
    }
    case MESG_ACKNOWLEDGED_DATA_ID:
    case MESG_BURST_DATA_ID:
    case MESG_BROADCAST_DATA_ID:
    {
        // The flagged and unflagged data messages have the same
        // message ID. Therefore, we need to check the size to
        // verify of a flag is present at the end of a message.
        // To enable flagged messages, must call ANT_RxExtMesgsEnable first.
        if (usSize_ > MESG_DATA_SIZE)
        {
            UCHAR ucFlag = stMessage.aucData[MESSAGE_BUFFER_DATA10_INDEX];

            if (ucFlag & ANT_LIB_CONFIG_MESG_OUT_INC_TIME_STAMP && ucFlag & ANT_EXT_MESG_BITFIELD_DEVICE_ID)
            {
                // In case we miss messages for 2 seconds or longer, we use the system time from the standard C time library to calculate rollovers
                time_t currentRxTime = time(NULL);
                if (currentRxTime - previousRxTime >= 2)
                {
                    ulNewEventTime += (currentRxTime - previousRxTime) / 2 * 32768;
                }
                previousRxTime = currentRxTime;

                unsigned short usCurrentEventTime = stMessage.aucData[MESSAGE_BUFFER_DATA15_INDEX] | (stMessage.aucData[MESSAGE_BUFFER_DATA16_INDEX] << 8);
                unsigned short usDeltaEventTime = usCurrentEventTime - usPreviousEventTime;
                ulNewEventTime += usDeltaEventTime;
                usPreviousEventTime = usCurrentEventTime;
                UE_LOG(LogTemp, Warning, TEXT("%f-"), (double)ulNewEventTime / 32768);

                // NOTE: In this example we use the incoming message timestamp as it typically has the most accuracy
                // NOTE: The library will handle the received time discrepancy caused by power only event count linked messages
                if (bPowerDecoderInitialized)
                {
                    DecodePowerMessage((double)ulNewEventTime / 32768, &stMessage.aucData[ucDataOffset]);
                }

                // NOTE: We must compensate for the power only event count/rx time discrepance here, because the library does not decode Te/Ps
                // The torque effectiveness/pedal smoothness page is tied to the power only page and vice versa,
                // so both pages share the same "received time" depending on which page was received first and if the event count updated.
                if (stMessage.aucData[ucDataOffset] == ANT_TEPS || stMessage.aucData[ucDataOffset] == ANT_POWERONLY)
                {
                    UCHAR ucNewPowerOnlyUpdateEventCount = stMessage.aucData[ucDataOffset + 1];

                    if (ucNewPowerOnlyUpdateEventCount != ucPowerOnlyUpdateEventCount)
                    {
                        ucPowerOnlyUpdateEventCount = ucNewPowerOnlyUpdateEventCount;
                        dRxTimeTePs = (double)ulNewEventTime / 32768;
                    }

                    if (stMessage.aucData[ucDataOffset] == ANT_TEPS)
                    {
                        // NOTE: Any value greater than 200 or 100% should be considered "INVALID"
                        FLOAT fLeftTorqueEffectiveness = (float)stMessage.aucData[ucDataOffset + 2] / 2;
                        FLOAT fRightTorqueEffectiveness = (float)stMessage.aucData[ucDataOffset + 3] / 2;
                        FLOAT fLeftOrCombPedalSmoothness = (float)stMessage.aucData[ucDataOffset + 4] / 2;
                        FLOAT fRightPedalSmoothness = (float)stMessage.aucData[ucDataOffset + 4] / 2;
                        TePsReceiver(dRxTimeTePs, fLeftTorqueEffectiveness, fRightTorqueEffectiveness, fLeftOrCombPedalSmoothness, fRightPedalSmoothness);
                    }
                    else
                    {
                        // NOTE: Power only is a separate data stream containing similar power data compared to torque data pages but containing pedal power balance
                        // On power only sensors, it would be valuable to average power balance between generated records
                        FLOAT fPowerBalance = (float)(0x7F & stMessage.aucData[ucDataOffset + 2]);
                        BOOL bPowerBalanceRightPedalIndicator = (0x80 & stMessage.aucData[ucDataOffset + 2]) != 0;
                        PowerBalanceReceiver(dRxTimeTePs, fPowerBalance, bPowerBalanceRightPedalIndicator);
                    }
                }
            }

            if (bDisplay && ucFlag & ANT_EXT_MESG_BITFIELD_DEVICE_ID)
            {
                // Channel ID of the device that we just recieved a message from.
                USHORT usDeviceNumber = stMessage.aucData[MESSAGE_BUFFER_DATA11_INDEX] | (stMessage.aucData[MESSAGE_BUFFER_DATA12_INDEX] << 8);
                UCHAR ucDeviceType = stMessage.aucData[MESSAGE_BUFFER_DATA13_INDEX];
                UCHAR ucTransmissionType = stMessage.aucData[MESSAGE_BUFFER_DATA14_INDEX];

                UE_LOG(LogTemp, Warning, TEXT("Chan ID(%d/%d/%d) - "), usDeviceNumber, ucDeviceType, ucTransmissionType);
            }
        }

        // Display recieved message
        bPrintBuffer = TRUE;
        ucDataOffset = MESSAGE_BUFFER_DATA2_INDEX;   // For most data messages

        if (bDisplay)
        {
            if (stMessage.ucMessageID == MESG_ACKNOWLEDGED_DATA_ID)
            {
                UE_LOG(LogTemp, Warning, TEXT("Acked Rx:(%d): "), stMessage.aucData[MESSAGE_BUFFER_DATA1_INDEX]);
            }
            else if (stMessage.ucMessageID == MESG_BURST_DATA_ID)
            {
                UE_LOG(LogTemp, Warning, TEXT("Burst(0x%02x) Rx:(%d): "), ((stMessage.aucData[MESSAGE_BUFFER_DATA1_INDEX] & 0xE0) >> 5), stMessage.aucData[MESSAGE_BUFFER_DATA1_INDEX] & 0x1F);
            }
            else
            {
                UE_LOG(LogTemp, Warning, TEXT("Rx:(%d): "), stMessage.aucData[MESSAGE_BUFFER_DATA1_INDEX]);
            }
        }
        break;
    }
    case MESG_EXT_BROADCAST_DATA_ID:
    case MESG_EXT_ACKNOWLEDGED_DATA_ID:
    case MESG_EXT_BURST_DATA_ID:
    {

        // The "extended" part of this message is the 4-byte channel
        // id of the device that we recieved this message from. This message
        // is only available on the AT3. The AP2 uses flagged versions of the
        // data messages as shown above.

        // Channel ID of the device that we just recieved a message from.
        USHORT usDeviceNumber = stMessage.aucData[MESSAGE_BUFFER_DATA2_INDEX] | (stMessage.aucData[MESSAGE_BUFFER_DATA3_INDEX] << 8);
        UCHAR ucDeviceType = stMessage.aucData[MESSAGE_BUFFER_DATA4_INDEX];
        UCHAR ucTransmissionType = stMessage.aucData[MESSAGE_BUFFER_DATA5_INDEX];

        bPrintBuffer = TRUE;
        ucDataOffset = MESSAGE_BUFFER_DATA6_INDEX;   // For most data messages

        if (bDisplay)
        {
            // Display the channel id
            UE_LOG(LogTemp, Warning, TEXT("Chan ID(%d/%d/%d) "), usDeviceNumber, ucDeviceType, ucTransmissionType);

            if (stMessage.ucMessageID == MESG_EXT_ACKNOWLEDGED_DATA_ID)
            {
                UE_LOG(LogTemp, Warning, TEXT("- Acked Rx:(%d): "), stMessage.aucData[MESSAGE_BUFFER_DATA1_INDEX]);
            }
            else if (stMessage.ucMessageID == MESG_EXT_BURST_DATA_ID)
            {
                UE_LOG(LogTemp, Warning, TEXT("- Burst(0x%02x) Rx:(%d): "), ((stMessage.aucData[MESSAGE_BUFFER_DATA1_INDEX] & 0xE0) >> 5), stMessage.aucData[MESSAGE_BUFFER_DATA1_INDEX] & 0x1F);
            }
            else
            {
                UE_LOG(LogTemp, Warning, TEXT("- Rx:(%d): "), stMessage.aucData[MESSAGE_BUFFER_DATA1_INDEX]);
            }
        }

        // NOTE: A different source of sub-second timing is required if the device does not support received time extended messages.

        break;
    }

    default:
    {
        break;
    }
    }

    // If we recieved a data message, diplay its contents here.
    if (bPrintBuffer)
    {
        if (bDisplay)
        {
            UE_LOG(LogTemp, Warning, TEXT("[%02x],[%02x],[%02x],[%02x],[%02x],[%02x],[%02x],[%02x]"),
                stMessage.aucData[ucDataOffset + 0],
                stMessage.aucData[ucDataOffset + 1],
                stMessage.aucData[ucDataOffset + 2],
                stMessage.aucData[ucDataOffset + 3],
                stMessage.aucData[ucDataOffset + 4],
                stMessage.aucData[ucDataOffset + 5],
                stMessage.aucData[ucDataOffset + 6],
                stMessage.aucData[ucDataOffset + 7]);
        }
        else
        {
            static int iIndex = 0;
            static char ac[] = { '|', '/', '-', '\\' };
            UE_LOG(LogTemp, Warning, TEXT("Rx: %c\r"), ac[iIndex++]); 
            fflush(stdout);
            iIndex &= 3;

        }
    }

    return;
}

void AAntPlusReaderActor::RecordReceiver(double dLastRecordTime_, double dTotalRotation_, double dTotalEnergy_, float fAverageCadence_, float fAveragePower_)
{
    //Handle new records from power recording library.
    UE_LOG(LogTemp, Warning, TEXT("%lf, %lf, %lf, %f, %f"),
        dLastRecordTime_, dTotalRotation_, dTotalEnergy_, fAverageCadence_, fAveragePower_);

    APower = fAveragePower_;

    ACadence = fAverageCadence_;
}

void AAntPlusReaderActor::TePsReceiver(double dRxTime_, float fLeftTorqEff_, float fRightTorqEff_, float fLeftOrCPedSmth_, float fRightPedSmth_)
{
    //Handle new torque effectivenessand pedal smoothness data page.
    UE_LOG(LogTemp, Warning, TEXT("RxTime,LTE,RTE,LCPS,RPS,%f, %f, %f, %f, %f"),
        dRxTime_, fLeftTorqEff_, fRightTorqEff_, fLeftOrCPedSmth_, fRightPedSmth_);
}

void AAntPlusReaderActor::PowerBalanceReceiver(double dRxTime_, float fPowerBalance_, bool bPowerBalanceRightPedalIndicator_)
{
    //Handle power balance from power only data page.
    UE_LOG(LogTemp, Warning, TEXT("RxTime,PwrBal,RightPedal,%f, %f, %d"),
        dRxTime_, fPowerBalance_, bPowerBalanceRightPedalIndicator_);
}

//==============================================
// Task for Threading

WaitForMessagesTask2::WaitForMessagesTask2(DSIFramerANT* pclMsgObj)
{
    pclMessageObject = pclMsgObj;
}

WaitForMessagesTask2::~WaitForMessagesTask2()
{
    UE_LOG(LogTemp, Warning, TEXT("Stopped Waiting For Messages!!"))
}

void WaitForMessagesTask2::DoWork()
{
    while (true)
    {
        ANT_MESSAGE stMessage;
        USHORT usSize;

        if (pclMessageObject->WaitForMessage(1000))
        {
            usSize = pclMessageObject->GetMessage(&stMessage);

            if (usSize == DSI_FRAMER_ERROR)
            {
                // Get the message to clear the error
                usSize = pclMessageObject->GetMessage(&stMessage, MESG_MAX_SIZE_VALUE);
                USBConnected = false;
            }

            else if (usSize != DSI_FRAMER_TIMEDOUT && usSize != 0)
            {
                AntPlusReaderActor->ProcessMessage(stMessage, usSize);
            }
        }
    }
}