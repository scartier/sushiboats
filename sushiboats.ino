#include <commlib.h>

byte faceOffsetArray[] = { 0, 1, 2, 3, 4, 5, 0, 1, 2, 3, 4, 5 };

#define CCW_FROM_FACE(f, amt) faceOffsetArray[6 + (f) - (amt)]
#define CW_FROM_FACE(f, amt)  faceOffsetArray[(f) + (amt)]
#define OPPOSITE_FACE(f)      CW_FROM_FACE((f), 3)

#define DONT_CARE 0

// ----------------------------------------------------------------------------------------------------

uint32_t randState = 0;

// ----------------------------------------------------------------------------------------------------

enum TileType : uint8_t
{
  TileType_NotPresent,    // no neighbor present
  TileType_Unknown,
  TileType_ConveyorEmpty,
  TileType_ConveyorPlate,
  TileType_Ingredient,
  TileType_Customer
};

// ----------------------------------------------------------------------------------------------------

enum IngredientType : uint8_t
{
  IngredientType_Empty,
  IngredientType_Tuna,       // red
  IngredientType_YellowTail, // pale pink
  IngredientType_Ebi,        // orange
  IngredientType_Unagi,      // brown
  IngredientType_Tamago,     // yellow
  IngredientType_Wasabi,     // light green
  IngredientType_Taro,       // purple

  IngredientType_MAX,
  IngredientType_MIN = IngredientType_Tuna,
};

struct IngredientInfo
{
  uint16_t color;
  byte pattern;
};

#define RGB_TO_U16(r,g,b) ((((uint16_t)(r)>>3) & 0x1F)<<1 | (((uint16_t)(g)>>3) & 0x1F)<<6 | (((uint16_t)(b)>>3) & 0x1F)<<11)

IngredientInfo ingredientInfo[IngredientType_MAX] =
{
  { RGB_TO_U16(  0,    0,   0 ), 0b000 },  // empty
  { RGB_TO_U16( 160,  32,   0 ), 0b011 },  // tuna
  { RGB_TO_U16( 192, 192,  96 ), 0b101 },  // yellowtail
  { RGB_TO_U16( 224, 160,   0 ), 0b111 },  // ebi
  { RGB_TO_U16( 112, 112,   0 ), 0b011 },  // unagi
  { RGB_TO_U16( 216, 144,   0 ), 0b111 },  // tamago
  { RGB_TO_U16(  16, 184,   0 ), 0b001 },  // wasabi
  { RGB_TO_U16( 128,   0, 128 ), 0b001 },  // taro
};

byte ingredientsInUse = 0;    // bit field indicating which ingredients are being used (bit 0 = valid)
byte newIngredientsInUse;     // used when updating from a neighbor

// ----------------------------------------------------------------------------------------------------

byte downstreamConveyorFace = 0;
byte upstreamConveyorFace = 0;

struct TileInfo
{
  TileType tileType;
  bool tileContentsValid;
  IngredientType tileContents[FACE_COUNT];
};

TileInfo tileInfo;
TileInfo neighborTileInfo[FACE_COUNT];
byte neighborFaceOffset[FACE_COUNT];

byte neighborCount = 0;
bool trackError = false;
byte waitingForPing = 0;
bool pingError = false;

#define PULSE_RATE 500
Timer pulseTimer;

// Variables involved in moving contents along the conveyor track
#define TRACK_MOVE_RATE 2000
Timer moveTimer;
int tileInfoPacketsRemaining[FACE_COUNT] = { 0, 0, 0, 0, 0, 0 };   // used to send multiple packets of data in one command
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
  Command_TileInfo,             // one tile is sending its contents to another

  Command_IngredientsInUse1,    // broadcast so all tiles know which ingredients are in use...
  Command_IngredientsInUse2,    // bitfield is 8-bits so need two packets
  Command_IngredientSelected,   // ingredient tile selected its ingredient type

  Command_FaceIndex,            // tell neighbor which of our faces is touching

  Command_AddIngredientToPlate, // ingredient tile was clicked - add it to all adjacent plates
  Command_ServeCustomer,        // when a conveyor plate matches an adjacent customer request

  Command_ResetAllTiles,        // player reset the game
};

// ====================================================================================================

void setup()
{
  reset();
}

// ----------------------------------------------------------------------------------------------------

void reset()
{
  // Reset our tile state
  tileInfo.tileType = TileType_Unknown;
  //clearTileContents(tileInfo.tileContents);
  tileInfo.tileContentsValid = false;

  // Reset our view of our neighbor tiles
  FOREACH_FACE(f)
  {
    neighborTileInfo[f].tileType = TileType_NotPresent;
  }
}

// ====================================================================================================
// COMMUNICATION
// ====================================================================================================

void broadcastToAllNeighbors(byte command, byte value)
{
  FOREACH_FACE(f)
  {
    if (isValueReceivedOnFaceExpired(f))
    {
      continue;
    }

    enqueueCommOnFace(f, command, value);
  }
}

// ----------------------------------------------------------------------------------------------------

void processCommForFace(byte commandByte, byte value, byte f)
{
  byte face;

  //pulseTimer.set(PULSE_RATE);

  // Use the comm system in a non-standard way to send multiple values for a command
  if (tileInfoPacketsRemaining[f] > 0)
  {
    // When sending tile info, both the command byte and the value byte are
    // valid pieces of data
    processCommand_TileInfo(commandByte, f);
    processCommand_TileInfo(value, f);
    if (tileInfoPacketsRemaining[f] <= 0)
    {
      if (f == upstreamConveyorFace)
      {
        // Got all the tile info - we're ready to switch when our neighbors are
        receivedUpstreamTileInfo = true;
        receivedUpstreamTileInfo_latch = true;
      }
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

    case Command_TileInfo:
      // Upstream neighbor responded to our request for their contents
      // We are abusing the comm system here to send more than one 4-byte value for the command
      tileInfoPacketsRemaining[f] = 7;  // tile type + 6 faces
      processCommand_TileInfo(value, f);
      break;

    case Command_IngredientsInUse1:
      newIngredientsInUse = value;
      break;

    case Command_IngredientsInUse2:
      newIngredientsInUse |= (value << 4);
      newIngredientsInUse |= 1;              // bit 0 = valid

      // If this is different from what we have then save it and broadcast it further
      // If it is the same then the broadcast stops here - prevents infinite comm packets
      if (newIngredientsInUse != ingredientsInUse)
      {
        ingredientsInUse = newIngredientsInUse;
        if (tileInfo.tileType == TileType_ConveyorEmpty || tileInfo.tileType == TileType_ConveyorPlate)
        {
          broadcastIngredientsInUse();
        }
      }
      break;

    case Command_IngredientSelected:
      // Only an ingredient tile would send this
      // Only a conveyor tile cares
      if (tileInfo.tileType == TileType_ConveyorEmpty || tileInfo.tileType == TileType_ConveyorPlate)
      {
        neighborTileInfo[f].tileType = TileType_Ingredient;
        neighborTileInfo[f].tileContents[0] = (IngredientType) value;
        neighborTileInfo[f].tileContentsValid = true;
        ingredientsInUse |= 1 << value;
        broadcastIngredientsInUse();
      }
      break;

    case Command_FaceIndex:
      // Neighbor is telling us which of its faces is touching our face
      // We use this to get implicit rotations correct
      face = OPPOSITE_FACE(f);
      neighborFaceOffset[f] = face - value + ((face > value) ? 0 : 6);
      break;

    case Command_AddIngredientToPlate:
      if (tileInfo.tileType == TileType_ConveyorPlate &&
          neighborTileInfo[f].tileType == TileType_Ingredient &&
          neighborTileInfo[f].tileContentsValid)
      {
        IngredientType ingredientIndex = neighborTileInfo[f].tileContents[0];
        byte pattern = ingredientInfo[ingredientIndex].pattern;
        FOREACH_FACE(i)
        {
          if (pattern & (1<<i))
          {
            byte face = CW_FROM_FACE(i, neighborFaceOffset[f]);
            tileInfo.tileContents[face] = ingredientIndex;
          }
        }
      }
      break;

    case Command_ServeCustomer:
      if (tileInfo.tileType == TileType_Customer)
      {
        tileInfo.tileContentsValid = false;
      }
      break;
  }
}

// ----------------------------------------------------------------------------------------------------

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

      // Seems like we're starting the game so init our random
      randState = millis();
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

// ----------------------------------------------------------------------------------------------------

void processCommand_AssignTrack(byte value, byte fromFace)
{
  // Once we know both prev and next tiles we can move to Play state
  upstreamConveyorFace = fromFace;

  if (tileInfo.tileType != TileType_Unknown)
  {
    // Looped back around to the start - done assigning track
    return;
  }

  // Seems like we're starting the game so init our random
  randState = millis();

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

// ----------------------------------------------------------------------------------------------------

void processCommand_TileInfo(byte value, byte fromFace)
{
  switch (tileInfoPacketsRemaining[fromFace])
  {
    case 7:
      neighborTileInfo[fromFace].tileType = (TileType) value;
      break;

    default:
      byte face = CW_FROM_FACE(fromFace, 6 - tileInfoPacketsRemaining[fromFace]);
      neighborTileInfo[fromFace].tileContents[face] = (IngredientType) value;
      break;
  }

  tileInfoPacketsRemaining[fromFace]--;
  if (tileInfoPacketsRemaining[fromFace] == 0)
  {
    neighborTileInfo[fromFace].tileContentsValid = true;
  }
}

// ====================================================================================================
// UTILITY
// ====================================================================================================

void clearTileContents(IngredientType *tileContents)
{
  FOREACH_FACE(f)
  {
    tileContents[f] = IngredientType_Empty;
  }
}

// ----------------------------------------------------------------------------------------------------

void broadcastIngredientsInUse()
{
  broadcastToAllNeighbors(Command_IngredientsInUse1, ingredientsInUse & 0xF);
  broadcastToAllNeighbors(Command_IngredientsInUse2, (ingredientsInUse >> 4) & 0xF);
}

// ----------------------------------------------------------------------------------------------------

// Random code partially copied from blinklib because there's no function
// to set an explicit seed, which we need so we can share/replay puzzles.
byte __attribute__((noinline)) randGetByte()
{
  // Algorithm "xor" from p. 4 of Marsaglia, "Xorshift RNGs"
  uint32_t x = randState;
  x ^= x << 13;
  x ^= x >> 17;
  x ^= x << 5;
  randState = x;
  return x & 0xFF;
}

// ----------------------------------------------------------------------------------------------------

byte __attribute__((noinline)) randRange(byte min, byte max)
{
  uint32_t val = randGetByte();
  uint32_t range = max - min;
  val = (val * range) >> 8;
  return val + min;
}

// ====================================================================================================
// LOOP
// ====================================================================================================

void loop()
{
  commReceive();

  detectNeighbors();
  switch (tileInfo.tileType)
  {
    case TileType_Unknown:
      loop_Unknown();
      break;

    case TileType_Ingredient:
      loop_Ingredient();
      break;

    case TileType_Customer:
      loop_Customer();
      break;

    case TileType_ConveyorEmpty:
    case TileType_ConveyorPlate:
      loop_Conveyor();
      break;
  }

  commSend();

  render();
}

// ----------------------------------------------------------------------------------------------------

void detectNeighbors()
{
  // Detect all neighbors, taking optional action on neighbors added/removed
  neighborCount = 0;
  FOREACH_FACE(f)
  {
    if (isValueReceivedOnFaceExpired(f))
    {
      // No neighbor
      
      // Reset the tile info packet count in case it was in the middle 
      tileInfoPacketsRemaining[f] = 0;
      
      // Was there was one previously?
      if (neighborTileInfo[f].tileType != TileType_NotPresent)
      {
        if (tileInfo.tileType == TileType_ConveyorEmpty || tileInfo.tileType == TileType_ConveyorPlate)
        {
          // Remove ingredients from the list of used
          if (neighborTileInfo[f].tileType == TileType_Ingredient && neighborTileInfo[f].tileContentsValid)
          {
            ingredientsInUse &= ~(1<<neighborTileInfo[f].tileContents[0]);
            broadcastIngredientsInUse();
          }
        }

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

        if (tileInfo.tileType == TileType_ConveyorEmpty || tileInfo.tileType == TileType_ConveyorPlate)
        {
          // Send the list of ingredients in use
          // Ingredient tiles will use this to select a non-repeat (Command_IngredientUsed).
          // Customer tiles will use this to generate an order and send it to us (Command_TileInfo).
          enqueueCommOnFace(f, Command_IngredientsInUse1, ingredientsInUse & 0xF);
          enqueueCommOnFace(f, Command_IngredientsInUse2, (ingredientsInUse >> 4) & 0xF);
        }
      }
      neighborCount++;
    }
  }

  if (neighborCount == 0)
  {
    ingredientsInUse = 0;
  }
}

// ----------------------------------------------------------------------------------------------------

void loop_Unknown()
{
  if (neighborCount == 0)
  {
    if (buttonDoubleClicked())
    {
      randState = millis();
      tileInfo.tileType = TileType_Ingredient;
      tileInfo.tileContentsValid = false;
    }
  }
  else
  {
    trackError = false;

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
}

// ----------------------------------------------------------------------------------------------------

void loop_Ingredient()
{
  if (neighborCount == 0)
  {
    tileInfo.tileContentsValid = false;

    if (buttonDoubleClicked())
    {
      randState = millis();
      tileInfo.tileType = TileType_Customer;
      tileInfo.tileContentsValid = false;
    }
  }
  else if (tileInfo.tileContentsValid == false)
  {
    // Wait until a neighbor tells us what ingredients are already in use
    // Valid bit == bit 0
    if (ingredientsInUse & 0x01)
    {
      // Assign ourself an ingredient based on what is not in use already

      // If all ingredients are in use then do nothing
      if (ingredientsInUse == 0xFF)
      {
        return;
      }

      // Grab a new ingredient type
      // For now just go sequentially
      // TODO : Maybe randomize?
      for (byte newIngredientIndex = IngredientType_MIN; newIngredientIndex < IngredientType_MAX; newIngredientIndex++)
      {
        if ((ingredientsInUse & (1<<newIngredientIndex)) == 0)
        {
          // Found the first unused ingredient - take it
          tileInfo.tileContentsValid = true;
          clearTileContents(tileInfo.tileContents);
          tileInfo.tileContents[0] = (IngredientType) newIngredientIndex;

          // Broadcast our choice so all the other tiles know
          // Conveyor tiles will merge it into the ingredientsInUse bitfield, and send it around the loop
          FOREACH_FACE(f)
          {
            if (!isValueReceivedOnFaceExpired(f))
            {
              enqueueCommOnFace(f, Command_IngredientSelected, newIngredientIndex);
              enqueueCommOnFace(f, Command_FaceIndex, f);
            }
          }
          break;
        }
      }
      
    }
  }
  else
  {
    // Normal gameplay
    if (buttonSingleClicked())
    {
      // Tell neighboring conveyors with plates to add our ingredient
      broadcastToAllNeighbors(Command_AddIngredientToPlate, DONT_CARE);
    }
  }
}

// ----------------------------------------------------------------------------------------------------

void loop_Customer()
{
  if (neighborCount == 0)
  {
    tileInfo.tileContentsValid = false;

    if (buttonDoubleClicked())
    {
      randState = millis();
      tileInfo.tileType = TileType_Unknown;
    }
  }
  else if (tileInfo.tileContentsValid == false)
  {
    // Wait until a neighbor tells us what ingredients are already in use
    // Valid bit == bit 0
    // Make sure there is at least one ingredient being used
    if (ingredientsInUse & 0x01)
    {
      byte numIngredientsInUse = 0;
      for (byte i = IngredientType_MIN; i < IngredientType_MAX; i++)
      {
        if (ingredientsInUse & (1<<i))
        {
          numIngredientsInUse++;
        }
      }
      
      if (numIngredientsInUse >= 1)
      {
        tileInfo.tileContentsValid = true;
        FOREACH_FACE(f)
        {
          tileInfo.tileContents[f] = IngredientType_Empty;
        }
        
        // Create an order based on the available ingredients
        byte numIngredientsInOrder = randRange(1, 4);
        for (byte ingredientNum = 0; ingredientNum < numIngredientsInOrder; ingredientNum++)
        {
          // Get a random ingredient
          byte ingredientsToSkip = randRange(0, numIngredientsInUse);
          for (byte i = IngredientType_MIN; i < IngredientType_MAX; i++)
          {
            if (ingredientsInUse & (1<<i))
            {
              if (ingredientsToSkip == 0)
              {
                // Use this ingredient
                byte firstFace = randRange(0, FACE_COUNT);
                byte pattern = ingredientInfo[i].pattern;
                FOREACH_FACE(f)
                {
                  if (pattern & 0x1)
                  {
                    byte face = CW_FROM_FACE(firstFace, f);
                    tileInfo.tileContents[face] = (IngredientType) i;
                  }
                  pattern >>= 1;
                }
                break;
              }
              else
              {
                ingredientsToSkip--;
              }
            }
          }
        }

        // Tell all our attached conveyor neighbors about our contents
        FOREACH_FACE(f)
        {
          // Must send all the info back-to-back as that is what the receiver will expect
          enqueueCommOnFace(f, Command_TileInfo, tileInfo.tileType);
          // Order doesn't matter since matching is rotation-agnostic
          enqueueCommOnFace(f, tileInfo.tileContents[0], tileInfo.tileContents[1]);
          enqueueCommOnFace(f, tileInfo.tileContents[2], tileInfo.tileContents[3]);
          enqueueCommOnFace(f, tileInfo.tileContents[4], tileInfo.tileContents[5]);
        }

      }
    }
  }
  else
  {
    // Normal gameplay
  }
}

// ----------------------------------------------------------------------------------------------------

void loop_Conveyor()
{
  conveyor_Move();

  if (buttonSingleClicked())
  {
    // Rotate the plate
    IngredientType contentSave = tileInfo.tileContents[0];
    for (byte f = 0; f <= 4; f++)
    {
      tileInfo.tileContents[f] = tileInfo.tileContents[f+1];
    }
    tileInfo.tileContents[5] = contentSave;
  }

  if (tileInfo.tileType == TileType_ConveyorPlate)
  {
    conveyor_AttemptOrderMatch();
  }
}

// ----------------------------------------------------------------------------------------------------

void conveyor_Move()
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
    enqueueCommOnFace(downstreamConveyorFace, Command_TileInfo, tileInfo.tileType);
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

    // Get ready for the next move
    requestedUpstreamTileInfo = false;
    receivedUpstreamTileInfo = false;
    sentDownstreamTileInfo = false;
    moveTimer.set(TRACK_MOVE_RATE);

    // Invalidate our view of the upstream tile since we just took it
    neighborTileInfo[upstreamConveyorFace].tileType = TileType_Unknown;
    neighborTileInfo[upstreamConveyorFace].tileContentsValid = false;
  }
}

// ----------------------------------------------------------------------------------------------------

void conveyor_AttemptOrderMatch()
{
  // Check if the contents of our plate matches an adjacent customer order
  FOREACH_FACE(f)
  {
    if (neighborTileInfo[f].tileType != TileType_Customer)
    {
      continue;
    }

    if (!tileInfo.tileContentsValid || !neighborTileInfo[f].tileContentsValid)
    {
      continue;
    }

    // Allow match at any rotation - I'm not cruel
    FOREACH_FACE(matchRotation)
    {
      // Start out assuming a match
      // Prove me wrong, kids. Prove me wrong.
      bool matched = true;
      FOREACH_FACE(matchFace)
      {
        byte orderFace = CW_FROM_FACE(matchFace, matchRotation);
        if (tileInfo.tileContents[matchFace] != neighborTileInfo[f].tileContents[orderFace])
        {
          matched = false;
          break;
        }
      }

      // If we matched then clear the plate and the customer
      if (matched)
      {
        // Clear the plate!
        clearTileContents(tileInfo.tileContents);

        // Clear our knowledge of the customer's order
        neighborTileInfo[f].tileContentsValid = false;

        // Tell the customer to clear their order (and get a new one)
        enqueueCommOnFace(f, Command_ServeCustomer, DONT_CARE);

        // Break out - plate can only be served once
        break;
      }
    }
  }
}

// ====================================================================================================

void render()
{
  Color color;

  setColor(OFF);

  if (!pulseTimer.isExpired())
  {
    byte pulseAmount = (pulseTimer.getRemaining() > 255) ? 255 : pulseTimer.getRemaining();
    setColor(makeColorRGB(pulseAmount, pingError ? 0 : pulseAmount, pingError ? 0 : pulseAmount));
  }

  switch (tileInfo.tileType)
  {
    case TileType_Unknown:
      if (neighborCount == 0)
      {
        setColor(makeColorRGB(128, 128, 128));
      }
      else
      {
        FOREACH_FACE(f)
        {
          if (!isValueReceivedOnFaceExpired(f))
          {
            setColorOnFace(trackError ? RED : WHITE, f);
          }
        }
      }
      break;

    case TileType_ConveyorEmpty:
      {
        color = makeColorRGB(16, 32, 32);
        setColorOnFace(color, downstreamConveyorFace);
        setColorOnFace(color, upstreamConveyorFace);
      }
      break;

    case TileType_ConveyorPlate:
      setColor(makeColorRGB(0, 32, 64));
      if (tileInfo.tileContentsValid)
      {
        FOREACH_FACE(f)
        {
          IngredientType ingredientType = tileInfo.tileContents[f];
          if (ingredientType != IngredientType_Empty)
          {
            color.as_uint16 = ingredientInfo[ingredientType].color;
            setColorOnFace(color, f);
          }
        }
      }
      break;

    case TileType_Ingredient:
      if (tileInfo.tileContentsValid)
      {
        IngredientType ingredientIndex = tileInfo.tileContents[0];
        color.as_uint16 = ingredientInfo[ingredientIndex].color;
        byte pattern = ingredientInfo[ingredientIndex].pattern;
        FOREACH_FACE(f)
        {
          if (pattern & (1<<f))
          {
            setColorOnFace(color, f);
          }
        }
      }
      else
      {
        setColorOnFace(makeColorRGB(128, 128, 0), 0);
      }
      break;

    case TileType_Customer:
      if (tileInfo.tileContentsValid)
      {
        FOREACH_FACE(f)
        {
          if (tileInfo.tileContents[f] != IngredientType_Empty)
          {
            color.as_uint16 = ingredientInfo[tileInfo.tileContents[f]].color;
            setColorOnFace(color, f);
          }
        }
      }
      else
      {
        setColorOnFace(makeColorRGB(128, 0, 128), 0);
      }
      break;
  }

  FOREACH_FACE(f)
  {
    if (commInsertionIndexes[f] == COMM_INDEX_ERROR_OVERRUN)
    {
      setColorOnFace(MAGENTA, f);
    }
  }

/*
    if (moveTimerExpired_latch) setColorOnFace(WHITE, 0);
    if (requestedUpstreamTileInfo_latch) setColorOnFace(WHITE, 1);
    if (receivedUpstreamTileInfo_latch) setColorOnFace(WHITE, 2);
    if (downstreamRequestedTileInfo_latch) setColorOnFace(WHITE, 3);
    if (sentDownstreamTileInfo_latch) setColorOnFace(WHITE, 4);
    */
}
