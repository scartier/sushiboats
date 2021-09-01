#include <commlib.h>

byte faceOffsetArray[] = { 0, 1, 2, 3, 4, 5, 0, 1, 2, 3, 4, 5 };

#define CCW_FROM_FACE(f, amt) faceOffsetArray[6 + (f) - (amt)]
#define CW_FROM_FACE(f, amt)  faceOffsetArray[(f) + (amt)]
#define OPPOSITE_FACE(f)      CW_FROM_FACE((f), 3)

#define DONT_CARE 0

// ----------------------------------------------------------------------------------------------------

enum GameState : uint8_t
{
  GameState_SetupTrack,   // tiles were just programmed - player is creating the conveyor track
  GameState_Play          // game is playing
};

enum TileType : uint8_t
{
  TileType_NotPresent,    // no neighbor present
  TileType_Unknown,
  TileType_ConveyorEmpty,
  TileType_ConveyorPlate,
  TileType_Ingredient,
  TileType_Customer
};

enum ContentType : uint8_t
{
  ContentType_Empty,
  ContentType_Salmon,
  ContentType_Tuna,
  ContentType_Taro,
  ContentType_Cucumber,
  ContentType_YellowTail,
  ContentType_Avocado,
  ContentType_Ebi,        // shrimp
  ContentType_Unagi,      // eel
  ContentType_Tamago,     // egg
};

GameState gameState = GameState_SetupTrack;
byte downstreamConveyorFace = 0;
byte upstreamConveyorFace = 0;

struct TileInfo
{
  TileType tileType;
  bool tileContentsValid;
  ContentType tileContents[FACE_COUNT];
};

TileInfo tileInfo;
TileInfo neighborTileInfo[FACE_COUNT];

bool trackError = false;
byte waitingForPing = 0;
bool pingError = false;

#define PULSE_RATE 500
Timer pulseTimer;

// Variables involved in moving contents along the conveyor track
#define TRACK_MOVE_RATE 2000
Timer moveTimer;
int tileInfoPacketsRemaining = 0;   // used to send multiple packets of data in one command
bool requestedUpstreamTileInfo = false;
bool receivedUpstreamTileInfo = false;
bool downstreamRequestedTileInfo = false;
bool sentDownstreamTileInfo = false;
bool doneOneMove = false;

bool moveTimerExpired_latch = false;
bool requestedUpstreamTileInfo_latch = false;
bool receivedUpstreamTileInfo_latch = false;
bool downstreamRequestedTileInfo_latch = false;
bool sentDownstreamTileInfo_latch = false;

// ----------------------------------------------------------------------------------------------------

enum Command : uint8_t
{
  Command_PingTrack,
  Command_AssignTrack,

  Command_RequestTileInfo,      // one tile is asking another for its tile type+contents
  Command_ResponseTileInfo,     // response

  Command_IngredientUsed,       // a new ingredient type is added to the track - so customers know
  Command_IngredientRemoved,    // an ingredient type was removed from the track - so customers know

  Command_AddIngredientToPlate, // ingredient tile was clicked - add it to all adjacent plates
  Command_ServeCustomer,        // when a conveyor plate matches an adjacent customer request

  Command_ResetAllTiles,        // player reset the game
};

// ----------------------------------------------------------------------------------------------------

void setup()
{
  reset();
}

void reset()
{
  gameState = GameState_SetupTrack;

  // Reset our tile state
  tileInfo.tileType = TileType_Unknown;
  clearTileContents(tileInfo.tileContents);
  tileInfo.tileContentsValid = true;

  // Reset our view of our neighbor tiles
  FOREACH_FACE(f)
  {
    neighborTileInfo[f].tileType = TileType_NotPresent;
  }
}

void processCommForFace(byte commandByte, byte value, byte f)
{
  // Use the comm system in a non-standard way to send multiple values for a command
  // TODO : Reset 'tileInfoPacketsRemaining' when neighbor is lost
  if (f == upstreamConveyorFace && tileInfoPacketsRemaining > 0)
  {
    // When sending tile info, both the command byte and the value byte are
    // valid pieces of data
    processCommand_ResponseTileInfo(commandByte, f);
    processCommand_ResponseTileInfo(value, f);
    if (tileInfoPacketsRemaining <= 0)
    {
      // Got all the tile info - we're ready to switch when our neighbors are
      receivedUpstreamTileInfo = true;
      receivedUpstreamTileInfo_latch = true;
    }
    return;
  }

  Command command = (Command) commandByte;

  switch (command)
  {
    case Command_PingTrack:
      processCommand_PingTrack(value, f);
      break;

    case Command_AssignTrack:
      processCommand_AssignTrack(value, f);
      break;

    case Command_RequestTileInfo:
      if (f == downstreamConveyorFace)
      {
        // Downstream neighbor is ready to accept our contents
        // We will send when we are also ready to move (might be immediately)
        downstreamRequestedTileInfo = true;
        downstreamRequestedTileInfo_latch = true;
      }
      break;

    case Command_ResponseTileInfo:
      // Upstream neighbor responded to our request for their contents
      // We are abusing the comm system here to send more than one 4-byte value for the command
      tileInfoPacketsRemaining = 7;
      processCommand_ResponseTileInfo(value, f);
      break;
  }
}

void processCommand_PingTrack(byte value, byte fromFace)
{
  pulseTimer.set(PULSE_RATE);

  pingError = false;
  if (value)
  {
    pingError = true;
  }

  // If we were the originator then consume the ping
  if (waitingForPing > 0)
  {
    // We were the originator of the ping - consume it
    waitingForPing--;

    // If we got both pings, and they were clean then we can start
    // assigning tile roles around the track
    if (waitingForPing == 0 && pingError == false)
    {
      // Assign ourself first as empty and start with the next tile as a plate
      tileInfo.tileType = TileType_ConveyorEmpty;
      tileInfo.tileContentsValid = true;
      clearTileContents(tileInfo.tileContents);

      enqueueCommOnFace(fromFace, Command_AssignTrack, 1);
      downstreamConveyorFace = fromFace;
      moveTimer.set(TRACK_MOVE_RATE);
    }
  }
  else
  {
    // Neighbor is checking for a valid track
    if (trackError)
    {
      if (value == 0)
      {
        // If we have a new error then send it back
        enqueueCommOnFace(fromFace, Command_PingTrack, 1);
      }
    }
    else
    {
      // No error - continue the ping to the next tile along the track
      FOREACH_FACE(f)
      {
        if (f != fromFace)
        {
          enqueueCommOnFace(f, Command_PingTrack, value);
        }
      }
    }
  }
}

void processCommand_AssignTrack(byte value, byte fromFace)
{
  // Once we know both prev and next tiles we can move to Play state
  gameState = GameState_Play;

  upstreamConveyorFace = fromFace;

  if (tileInfo.tileType != TileType_Unknown)
  {
    // Looped back around to the start - done assigning track
    return;
  }

  // Every third conveyor tile has a plate, starting with the second tile
  // It is done this way so there aren't two plates next to one another when the loop completes
  tileInfo.tileType = (value == 1) ? TileType_ConveyorPlate : TileType_ConveyorEmpty;
  tileInfo.tileContentsValid = true;
  clearTileContents(tileInfo.tileContents);

  value = (value == 0) ? 2 : (value - 1);

  FOREACH_FACE(f)
  {
    if (!isValueReceivedOnFaceExpired(f) && f != fromFace)
    {
      downstreamConveyorFace = f;
      enqueueCommOnFace(f, Command_AssignTrack, value);
      break;
    }
  }

  // Start moving
  moveTimer.set(TRACK_MOVE_RATE);
}

void processCommand_ResponseTileInfo(byte value, byte fromFace)
{
  switch (tileInfoPacketsRemaining)
  {
    case 7:
      neighborTileInfo[fromFace].tileType = (TileType) value;
      break;

    default:
      byte face = CW_FROM_FACE(fromFace, 6 - tileInfoPacketsRemaining);
      neighborTileInfo[fromFace].tileContents[face] = (ContentType) value;
      break;
  }

  tileInfoPacketsRemaining--;
}

void loop()
{
  commReceive();

  if (gameState == GameState_SetupTrack)
  {
    loopSetupTrack();
  }
  else
  {
    switch (tileInfo.tileType)
    {
      case TileType_ConveyorEmpty:
      case TileType_ConveyorPlate:
        loopPlay_Conveyor();
        break;
    }
  }

  commSend();

  render();
}

void loopSetupTrack()
{
  byte neighborCount = 0;
  trackError = false;

  // Detect all neighbors, querying for unknown neighbor types
  FOREACH_FACE(f)
  {
    if (isValueReceivedOnFaceExpired(f))
    {
      // No neighbor
      // Was there was one previously?
      if (neighborTileInfo[f].tileType != TileType_NotPresent)
      {
        neighborTileInfo[f].tileType = TileType_NotPresent;
      }
    }
    else
    {
      // Neighbor present
      // Is this a new neighbor?
      if (neighborTileInfo[f].tileType == TileType_NotPresent)
      {
        neighborTileInfo[f].tileType = TileType_Unknown;
        neighborTileInfo[f].tileContentsValid = false;
      }
      neighborCount++;
    }
  }

  if (neighborCount != 2)
  {
    trackError = true;
  }

  // Check if two neighbors are adjacent
  FOREACH_FACE(f)
  {
    byte cwFace = CW_FROM_FACE(f, 1);
    byte ccwFace = CCW_FROM_FACE(f, 1);
    if ((neighborTileInfo[f].tileType != TileType_NotPresent) &&
        ((neighborTileInfo[cwFace].tileType != TileType_NotPresent) ||
         (neighborTileInfo[ccwFace].tileType != TileType_NotPresent)))
    {
      trackError = true;
    }
  }

  // Double click sends a pulse around the track, checking for any errors
  if (buttonDoubleClicked())
  {
    // Don't bother checking if there's an error on this tile
    if (!trackError)
    {
      pingError = false;
      pulseTimer.set(PULSE_RATE);

      FOREACH_FACE(f)
      {
        if (neighborTileInfo[f].tileType != TileType_NotPresent)
        {
          waitingForPing++;
          enqueueCommOnFace(f, Command_PingTrack, 0);
        }
      }
    }
  }

}

void loopPlay_Conveyor()
{
  if (!moveTimer.isExpired())
  {
    return;
  }

  moveTimerExpired_latch = true;

  // Move timer has expired
  // Start the process to move contents along the track

  // Timer expiration possibilities - upstream tile, this tile, downstream tile
  // There may be a delay between each line. Actions on the same line are performed without delay.
  //
  // THIS, UP, DOWN :
  //    THIS requests tile info from UP
  //    UP sends tile info to UP - THIS saves in neighborTileInfo
  //    DOWN requests tile info from THIS - THIS sends tile info to DOWN - SWITCH
  // UP, THIS, DOWN :
  //    THIS requests tile info from UP - UP sends tile info to UP - THIS saves in neighborTileInfo
  //    DOWN requests tile info from THIS - THIS sends tile info to DOWN - SWITCH
  // THIS, DOWN, UP :
  //    THIS requests tile info from UP
  //    DOWN requests tile info from THIS - THIS sends tile info to DOWN
  //    UP sends tile info to UP - THIS saves in neighborTileInfo - SWITCH
  // DOWN, THIS, UP :
  //    DOWN requests tile info from THIS
  //    THIS requests tile info from UP - THIS sends tile info to DOWN
  //    UP sends tile info to UP - THIS saves in neighborTileInfo - SWITCH
  // DOWN, UP, THIS :
  //    DOWN requests tile info from THIS
  //    THIS requests tile info from UP - THIS sends tile info to DOWN - UP sends tile info to UP - THIS saves in neighborTileInfo - SWITCH
  // UP, DOWN, THIS :
  //    DOWN requests tile info from THIS
  //    THIS requests tile info from UP - THIS sends tile info to DOWN - UP sends tile info to UP - THIS saves in neighborTileInfo - SWITCH

  // If the downstream neighbor has requested our contents then send that now
  if (downstreamRequestedTileInfo && !sentDownstreamTileInfo && commInsertionIndexes[downstreamConveyorFace] == 0)
  {
    // Must send all the info back-to-back as that is what the receiver will expect
    enqueueCommOnFace(downstreamConveyorFace, Command_ResponseTileInfo, tileInfo.tileType);
    // Start with the contents on the face that will receive the packet
    // ex. Sending to face 2 means starting with face 5 content and proceeding clockwise
    byte contentIndex1 = OPPOSITE_FACE(downstreamConveyorFace);
    byte contentIndex2 = CW_FROM_FACE(contentIndex1, 1);
    enqueueCommOnFace(downstreamConveyorFace, tileInfo.tileContents[contentIndex1], tileInfo.tileContents[contentIndex2]);
    contentIndex1 = CW_FROM_FACE(contentIndex2, 1);
    contentIndex2 = CW_FROM_FACE(contentIndex1, 1);
    enqueueCommOnFace(downstreamConveyorFace, tileInfo.tileContents[contentIndex1], tileInfo.tileContents[contentIndex2]);
    contentIndex1 = CW_FROM_FACE(contentIndex2, 1);
    contentIndex2 = CW_FROM_FACE(contentIndex1, 1);
    enqueueCommOnFace(downstreamConveyorFace, tileInfo.tileContents[contentIndex1], tileInfo.tileContents[contentIndex2]);

    sentDownstreamTileInfo = true;
    sentDownstreamTileInfo_latch = true;
  }

  // If this is our first loop being ready then request our upstream neighbor's tile info
  // Either they will send it immediately or whenever their timer has also expired
  if (!requestedUpstreamTileInfo)
  {
    enqueueCommOnFace(upstreamConveyorFace, Command_RequestTileInfo, DONT_CARE);
    requestedUpstreamTileInfo = true;
    requestedUpstreamTileInfo_latch = true;
  }
  
  // If we received the upstream tile info and sent ours along to the downstream tile then make the switch
  if (!doneOneMove && receivedUpstreamTileInfo && sentDownstreamTileInfo)
  {
    // Copy and invalidate the upstream neighbor's tile info
    tileInfo.tileType = neighborTileInfo[upstreamConveyorFace].tileType;
    FOREACH_FACE(f)
    {
      tileInfo.tileContents[f] = neighborTileInfo[upstreamConveyorFace].tileContents[f];
    }

    // Send upstream neighbor a message so they know to clear their contents
    //enqueueCommOnFace(upstreamConveyorFace, Command_TookContents, DONT_CARE);

    // Get ready for the next move
    requestedUpstreamTileInfo = false;
    receivedUpstreamTileInfo = false;
    sentDownstreamTileInfo = false;
    moveTimer.set(TRACK_MOVE_RATE);

    // Invalidate our view of the upstream tile since we just took it
    neighborTileInfo[upstreamConveyorFace].tileType = TileType_Unknown;
    neighborTileInfo[upstreamConveyorFace].tileContentsValid = false;

    //doneOneMove = true;
  }
}

void clearTileContents(ContentType *tileContents)
{
  FOREACH_FACE(f)
  {
    tileContents[f] = ContentType_Empty;
  }
}

void render()
{
  setColor(OFF);
  if (!pulseTimer.isExpired())
  {
    byte pulseAmount = (pulseTimer.getRemaining() > 255) ? 255 : pulseTimer.getRemaining();
    setColor(makeColorRGB(pulseAmount, pingError ? 0 : pulseAmount, pingError ? 0 : pulseAmount));
  }

  if (gameState == GameState_SetupTrack)
  {
    if (tileInfo.tileType == TileType_Unknown)
    {
      FOREACH_FACE(f)
      {
        if (!isValueReceivedOnFaceExpired(f))
        {
          setColorOnFace(trackError ? RED : WHITE, f);
        }
      }
    }
  }
  else
  {
    if (tileInfo.tileType == TileType_ConveyorEmpty)
    {
      Color trackColor = makeColorRGB(16, 32, 64);
      setColorOnFace(trackColor, downstreamConveyorFace);
      setColorOnFace(trackColor, upstreamConveyorFace);
    }
    else if (tileInfo.tileType == TileType_ConveyorPlate)
    {
      setColor(makeColorRGB(16, 128, 128));
    }
/*
    if (moveTimerExpired_latch) setColorOnFace(WHITE, 0);
    if (requestedUpstreamTileInfo_latch) setColorOnFace(WHITE, 1);
    if (receivedUpstreamTileInfo_latch) setColorOnFace(WHITE, 2);
    if (downstreamRequestedTileInfo_latch) setColorOnFace(WHITE, 3);
    if (sentDownstreamTileInfo_latch) setColorOnFace(WHITE, 4);
    */
  }
}
