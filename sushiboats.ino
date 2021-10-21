#include <commlib.h>

byte faceOffsetArray[] = { 0, 1, 2, 3, 4, 5, 0, 1, 2, 3, 4, 5 };

#define CCW_FROM_FACE(f, amt) faceOffsetArray[6 + (f) - (amt)]
#define CW_FROM_FACE(f, amt)  faceOffsetArray[(f) + (amt)]
#define OPPOSITE_FACE(f)      CW_FROM_FACE((f), 3)

#define DONT_CARE 0

// ----------------------------------------------------------------------------------------------------

uint32_t randState = 123;

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

// Used by conveyor tiles to track which face is connected to other conveyor tiles
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
bool hasAdjacentNeighbors = 0;
bool trackError = false;
byte waitingForPing = 0;
bool pingError = false;
byte conveyorFace;              // Face of attached conveyor (for ingredient tiles)

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
    if (!isValueReceivedOnFaceExpired(f))
    {
      enqueueCommOnFace(f, command, value);
    }
  }
}

// ----------------------------------------------------------------------------------------------------

void processCommForFace(byte commandByte, byte value, byte f)
{
  byte face;

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
      }
      break;

    case Command_TileInfo:
      // Upstream neighbor responded to our request for their contents
      // We are abusing the comm system here to send more than one 4-byte value for the command
      tileInfoPacketsRemaining[f] = 7;  // tile type + 6 faces
      processCommand_TileInfo(value, f);
      break;

    case Command_IngredientsInUse1:
      conveyorFace = f;   // save the face we got this from in case we are an ingredient tile
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
      // Received from an ingredient tile when the player clicks the button
      // The ingredient is added to all adjacent plates
      if (tileInfo.tileType == TileType_ConveyorPlate &&
          neighborTileInfo[f].tileType == TileType_Ingredient &&
          neighborTileInfo[f].tileContentsValid &&
          !sentDownstreamTileInfo)
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
      // Sent from a plate tile to a customer tile when the customer's
      // order is served. Forces the customer to invalidate their
      // current order, which will cause them to generate a new one.
      if (tileInfo.tileType == TileType_Customer)
      {
        if (tileInfo.tileContentsValid)
        {
          // TODO : Some animation showing the order going away
        }

        tileInfo.tileContentsValid = false;
      }
      break;
  }
}

// ----------------------------------------------------------------------------------------------------

void processCommand_PingTrack(byte value, byte fromFace)
{
  pulseTimer.set(PULSE_RATE);

  // Start by taking the error value from the sender
  // If there is an error on *this* tile then we will set this
  pingError = value;

  // If we were the originator then consume the ping
  if (waitingForPing > 0)
  {
    // We were the originator of the ping - consume it
    waitingForPing--;

    // If we got both pings, and they were clean, then we can start
    // assigning tile roles around the track
    if (waitingForPing == 0 && pingError == false)
    {
      // Assign ourself first as empty and start with the next tile as a plate
      tileInfo.tileType = TileType_ConveyorEmpty;
      tileInfo.tileContentsValid = true;
      clearTileContents(tileInfo.tileContents);

      enqueueCommOnFace(fromFace, Command_AssignTrack, 1);  // 1 = plate
      downstreamConveyorFace = fromFace;
      moveTimer.set(TRACK_MOVE_RATE);
    }
  }
  else  // (waitingForPing > 0)
  {
    // Someone in the track is checking if it's valid
    if (trackError)
    {
      if (!pingError)
      {
        // If we have a new error then send it back from whence it came
        enqueueCommOnFace(fromFace, Command_PingTrack, 1);
      }
    }
    else
    {
      // No error on this tile - propagate the ping to the next tile along the track
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

  // Every third conveyor tile has a plate, starting with the second tile.
  // It is done this way so there aren't two plates next to one another anywhere in the loop.
  // If we started with a plate then we might also end with a plate and thus
  // have two plates next to one another.
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
  broadcastToAllNeighbors(Command_IngredientsInUse1, ingredientsInUse & 0xE);   // don't send valid bit[0]
  broadcastToAllNeighbors(Command_IngredientsInUse2, (ingredientsInUse >> 4) & 0xF);
}

// ----------------------------------------------------------------------------------------------------

// Random code partially copied from blinklib because there's no function
// to set an explicit seed.
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
  byte val = randGetByte();
  byte range = max - min;
  uint16_t mult = val * range;
  return min + (mult >> 8);
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
  hasAdjacentNeighbors = false;
  bool prevFaceHasNeighbor = !isValueReceivedOnFaceExpired(5);
  FOREACH_FACE(f)
  {
    if (isValueReceivedOnFaceExpired(f))
    {
      // No neighbor
      
      // Reset the tile info packet count in case it was in the middle of receiving
      tileInfoPacketsRemaining[f] = 0;
      
      // If there was an ingredient then remove it from the ones in use
      if (neighborTileInfo[f].tileType == TileType_Ingredient && neighborTileInfo[f].tileContentsValid)
      {
        if (tileInfo.tileType == TileType_ConveyorEmpty || tileInfo.tileType == TileType_ConveyorPlate)
        {
          // Remove ingredients from the list of used
          ingredientsInUse &= ~(1<<neighborTileInfo[f].tileContents[0]);
          broadcastIngredientsInUse();
        }
      }

      neighborTileInfo[f].tileType = TileType_NotPresent;

      prevFaceHasNeighbor = false;
    }
    else
    {
      // Neighbor present

      // Set a flag when two neighbors are adjacent
      // This is used to determine track errors later
      if (prevFaceHasNeighbor)
      {
        hasAdjacentNeighbors = true;
      }

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
          enqueueCommOnFace(f, Command_IngredientsInUse1, ingredientsInUse & 0xE);
          enqueueCommOnFace(f, Command_IngredientsInUse2, (ingredientsInUse >> 4) & 0xF);
        }
      }
      neighborCount++;

      prevFaceHasNeighbor = true;
    }
  }

  // When not next to any other tiles, clear the valid bit in our ingredients
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

    if (hasAdjacentNeighbors)
    {
      trackError = true;
    }

    /*
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
    */

    // Double click sends a pulse around the track, checking for any errors
    if (buttonDoubleClicked())
    {
      // Don't bother checking if there's an error on this tile
      if (!trackError)
      {
        pingError = false;
        pulseTimer.set(PULSE_RATE);

        waitingForPing += 2;
        FOREACH_FACE(f)
        {
          enqueueCommOnFace(f, Command_PingTrack, 0);
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
          tileInfo.tileContents[0] = (IngredientType) newIngredientIndex;

          // Tell an adjacent conveyor tile our choice so it can merge it into the
          // the ingredientsInUse bitfield and propagate it everywhere.
          // We know this is the face of a conveyor because it previously told us
          // the original newIngredientsInUse.
          enqueueCommOnFace(conveyorFace, Command_IngredientSelected, newIngredientIndex);

          // Also tell it the face we're using to communicate so it can get relative rotations correct
          enqueueCommOnFace(conveyorFace, Command_FaceIndex, conveyorFace);

          // Got the ingredient - nothing else to do
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
      tileInfo.tileType = TileType_Unknown;
    }
  }
  else if (tileInfo.tileContentsValid == false)
  {
    // Wait until a neighbor tells us what ingredients are already in use
    // Valid bit == bit 0
    if (ingredientsInUse & 0x01)
    {
      // Count the number of ingredients available around the track
      byte numIngredientsInUse = 0;
      for (byte i = IngredientType_MIN; i < IngredientType_MAX; i++)
      {
        if (ingredientsInUse & (1<<i))
        {
          numIngredientsInUse++;
        }
      }
      
      // Make sure there is at least one ingredient being used
      if (numIngredientsInUse >= 1)
      {
        tileInfo.tileContentsValid = true;

        // Start the order empty
        clearTileContents(tileInfo.tileContents);
        
        // Create an order based on the available ingredients
        byte numIngredientsInOrder = randRange(2, 4);
        for (byte ingredientNum = 0; ingredientNum < numIngredientsInOrder; ingredientNum++)
        {
          // Get a random ingredient
          byte ingredientsToSkip = randRange(0, numIngredientsInUse);
          for (byte i = IngredientType_MIN; i < IngredientType_MAX; i++)
          {
            // Find an ingredient in use
            if (ingredientsInUse & (1<<i))
            {
              // Keep going until we get to our randomly-selected ingredient
              if (ingredientsToSkip > 0)
              {
                ingredientsToSkip--;
              }
              else
              {
                // Use this ingredient

                // Rotate it randomly
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
            }
          }
        }

        // Finished constructing the order
        // Tell all our attached conveyor neighbors about our contents
        FOREACH_FACE(f)
        {
          // Must send all the info back-to-back as that is what the receiver will expect
          enqueueCommOnFace(f, Command_TileInfo, tileInfo.tileType);
          // Doesn't matter what face we start with since matching is rotation-agnostic
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

  // Early out if this isn't a plate tile
  if (tileInfo.tileType != TileType_ConveyorPlate)
  {
    return;
  }

  // Check if this plate matches any adjacent customer orders
  conveyor_AttemptOrderMatch();

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
  else if (buttonDoubleClicked())
  {
    // Clear the plate
    FOREACH_FACE(f)
    {
      tileInfo.tileContents[f] = IngredientType_Empty;
    }
  }
}

// ----------------------------------------------------------------------------------------------------

void conveyor_Move()
{
  if (!moveTimer.isExpired())
  {
    return;
  }

  // Move timer has expired
  // Start the process to move contents along the track

  // Timer expiration possibilities - upstream tile, this tile, downstream tile
  // There may be a delay between each line. Actions on the same line are performed without delay.
  //
  // THIS, UP, DOWN :
  //    THIS requests tile info from UP
  //    UP sends tile info to THIS - THIS saves in neighborTileInfo
  //    DOWN requests tile info from THIS - THIS sends tile info to DOWN - SWITCH
  // UP, THIS, DOWN :
  //    THIS requests tile info from UP - UP sends tile info to THIS - THIS saves in neighborTileInfo
  //    DOWN requests tile info from THIS - THIS sends tile info to DOWN - SWITCH
  // THIS, DOWN, UP :
  //    THIS requests tile info from UP
  //    DOWN requests tile info from THIS - THIS sends tile info to DOWN
  //    UP sends tile info to THIS - THIS saves in neighborTileInfo - SWITCH
  // DOWN, THIS, UP :
  //    DOWN requests tile info from THIS
  //    THIS requests tile info from UP - THIS sends tile info to DOWN
  //    UP sends tile info to THIS - THIS saves in neighborTileInfo - SWITCH
  // DOWN, UP, THIS :
  //    DOWN requests tile info from THIS
  //    THIS requests tile info from UP - THIS sends tile info to DOWN - UP sends tile info to THIS - THIS saves in neighborTileInfo - SWITCH
  // UP, DOWN, THIS :
  //    DOWN requests tile info from THIS
  //    THIS requests tile info from UP - THIS sends tile info to DOWN - UP sends tile info to THIS - THIS saves in neighborTileInfo - SWITCH

  // If the downstream neighbor has requested our contents then send that now
  if (downstreamRequestedTileInfo && !sentDownstreamTileInfo && commInsertionIndexes[downstreamConveyorFace] == 0)
  {
    // Must send all the info back-to-back as that is what the receiver will expect
    enqueueCommOnFace(downstreamConveyorFace, Command_TileInfo, tileInfo.tileType);
    // Start with the contents on the face that will receive the packet
    // ex. Sending to face 2 means starting with face 5 content and proceeding clockwise
    byte *startingFace = &faceOffsetArray[OPPOSITE_FACE(downstreamConveyorFace)];
    enqueueCommOnFace(downstreamConveyorFace, tileInfo.tileContents[startingFace[0]], tileInfo.tileContents[startingFace[1]]);
    enqueueCommOnFace(downstreamConveyorFace, tileInfo.tileContents[startingFace[2]], tileInfo.tileContents[startingFace[3]]);
    enqueueCommOnFace(downstreamConveyorFace, tileInfo.tileContents[startingFace[4]], tileInfo.tileContents[startingFace[5]]);

    sentDownstreamTileInfo = true;
  }

  // If this is our first loop being ready then request our upstream neighbor's tile info
  // Either they will send it immediately or whenever their timer has also expired
  if (!requestedUpstreamTileInfo)
  {
    enqueueCommOnFace(upstreamConveyorFace, Command_RequestTileInfo, DONT_CARE);
    requestedUpstreamTileInfo = true;
  }
  
  // If we received the upstream tile info AND sent ours along to the downstream tile then make the switch
  if (receivedUpstreamTileInfo && sentDownstreamTileInfo)
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
    downstreamRequestedTileInfo = false;
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
}
