# Slab Allocator
A high-performance single-thread memory allocator using slab allocation technique, designed for efficient fixed-size memory allocations.

## Features

- **Api**: Provides both C and CPP interfaces
- **Slab-based allocation**: Organizes memory into slabs containing fixed-size units
- **Fast allocation/deallocation**: O(1) average case performance for both operations
- **Memory efficiency**: Minimizes fragmentation through fixed-size blocks
- **Automatic slab management**: Creates new slabs when needed and reclaims empty ones
- **Debug support**: Includes extensive error checking and statistics reporting

## License

MIT License

See [LICENSE](LICENSE) file for full license text.

## Implementation Details

### Key Components

1. **SlabUnit**:
   - Represents an individual allocation unit
   - Contains metadata (index and offset) and payload space
   - Provides conversion between unit and payload pointers

2. **SlabBlock**:
   - Manages a block of 64 units
   - Uses bitmaps to track free/allocated units
   - Implements unit allocation and block creation/destruction
   - Maintains linked list connections

3. **SlabAllocator**:
   - Main allocator class managing multiple slabs
   - Handles memory allocation and deallocation requests
   - Manages slab creation and reclamation
   - Maintains cache optimization

### Technical Specifications

- Fixed unit sizes (configurable at creation, up to 1024 bytes)
- 64 units per slab block
- Bitmap-based free unit tracking (64-bit)
- Automatic slab creation when needed
- Intelligent slab reclamation when empty
- Reduce traversal overhead
- Memory alignment to 8-byte boundaries

## Usage

### Basic Operations

```cpp
#include "./src/objectPool.hpp"
// Create an allocator for 256-byte units
slab::FixedAllocator allocator(256);

// Allocate memory
void* ptr1 = allocator.allocate();
void* ptr2 = allocator.allocate();

// Use the memory
int* data1 = static_cast<int*>(ptr1);
*data1 = 42;

// Deallocate memory
allocator.deallocate(ptr1);
allocator.deallocate(ptr2);

// Get statistics
std::cout << "Total slabs: " << allocator.total() << std::endl;
std::cout << "Reserved slabs: " << allocator.reserved() << std::endl;

// Print detailed statistics
allocator.print_stats();
```

## Performance Considerations

- The allocator maintains a "full" and a "work" list to minimize traversal
- Empty slabs beyond the reserved limit are automatically destroyed