/* ScummVM - Graphic Adventure Engine
 *
 * ScummVM is the legal property of its developers, whose names
 * are too numerous to list here. Please refer to the COPYRIGHT
 * file distributed with this source distribution.
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 */

#include "sci/sci.h"
#include "sci/resource/resource.h"
#include "sci/engine/seg_manager.h"
#include "sci/engine/script.h"
#include "sci/engine/state.h"
#include "sci/engine/selector.h"
#include "sci/engine/kernel.h"
#ifdef ENABLE_SCI32
#include "sci/engine/features.h"
#include "sci/sound/audio32.h"
#endif

#include "common/file.h"

namespace Sci {

// Loads arbitrary resources of type 'restype' with resource numbers 'resnrs'
// This implementation ignores all resource numbers except the first one.
reg_t kLoad(EngineState *s, int argc, reg_t *argv) {
	ResourceType restype = g_sci->getResMan()->convertResType(argv[0].toUint16());
	int resnr = argv[1].toUint16();

	// Request to dynamically allocate hunk memory for later use
	if (restype == kResourceTypeMemory)
		return s->_segMan->allocateHunkEntry("kLoad()", resnr);

	return make_reg(0, ((restype << 11) | resnr)); // Return the resource identifier as handle
}

// Unloads an arbitrary resource of type 'restype' with resource number 'resnr'
//  behavior of this call didn't change between sci0->sci1.1 parameter wise, which means getting called with
//  1 or 3+ parameters is not right according to sierra sci
reg_t kUnLoad(EngineState *s, int argc, reg_t *argv) {
	// NOTE: Locked resources in SSCI could be disposed by kUnLoad regardless
	// of lock state. With this ScummVM implementation of kUnLoad, game scripts
	// that dispose locked resources via kUnLoad without unlocking them with
	// kLock will leak the resource until the engine is restarted.

	ResourceType restype = g_sci->getResMan()->convertResType(argv[0].toUint16());
	reg_t resnr = argv[1];

	if (restype == kResourceTypeMemory)
		s->_segMan->freeHunkEntry(resnr);

	return s->r_acc;
}

reg_t kLock(EngineState *s, int argc, reg_t *argv) {
	// NOTE: In SSCI, kLock uses a boolean lock flag, not a lock counter.
	// ScummVM's current counter-based implementation should be better than SSCI
	// at dealing with game scripts that unintentionally lock & unlock the same
	// resource multiple times (e.g. through recursion), but it will introduce
	// memory bugs (resource leaks lasting until the engine is restarted, or
	// destruction of kernel locks that lead to a use-after-free) that are
	// masked by ResourceManager's LRU cache if scripts rely on kLock being
	// idempotent like it was in SSCI.
	//
	// Like SSCI, resource locks are not persisted in save games in ScummVM
	// until GK2, so it is also possible that kLock bugs will appear only after
	// restoring a save game.
	//
	// See also kUnLoad.

	ResourceType type = g_sci->getResMan()->convertResType(argv[0].toUint16());
	if (type == kResourceTypeSound && getSciVersion() >= SCI_VERSION_1_1) {
		type = g_sci->_soundCmd->getSoundResourceType(argv[1].toUint16());
	}

	const ResourceId id(type, argv[1].toUint16());
	const bool lock = argc > 2 ? argv[2].toUint16() : true;

#ifdef ENABLE_SCI32
	// SSCI GK2+SCI3 also saves lock states for View, Pic, and Sync resources,
	// but so far it seems like audio resources are the only ones that actually
	// need to be handled
	if (g_sci->_features->hasSci3Audio() && type == kResourceTypeAudio) {
		g_sci->_audio32->lockResource(id, lock);
		return s->r_acc;
	}
#endif

	if (getSciVersion() == SCI_VERSION_1_1 &&
		(type == kResourceTypeAudio36 || type == kResourceTypeSync36)) {
		return s->r_acc;
	}

	if (lock) {
		g_sci->getResMan()->findResource(id, true);
	} else {
		if (getSciVersion() < SCI_VERSION_2 && id.getNumber() == 0xFFFF) {
			// Unlock all resources of the requested type
			Common::List<ResourceId> resources = g_sci->getResMan()->listResources(type);
			Common::List<ResourceId>::iterator itr;
			for (itr = resources.begin(); itr != resources.end(); ++itr) {
				Resource *res = g_sci->getResMan()->testResource(*itr);
				if (res->isLocked())
					g_sci->getResMan()->unlockResource(res);
			}
		} else {
			Resource *which = g_sci->getResMan()->findResource(id, false);

			if (which)
				g_sci->getResMan()->unlockResource(which);
			else {
				if (id.getType() == kResourceTypeInvalid)
				  warning("[resMan] Attempt to unlock resource %i of invalid type %i", id.getNumber(), argv[0].toUint16());
				else
					// Happens in CD games (e.g. LSL6CD) with the message
					// resource. It isn't fatal, and it's usually caused
					// by leftover scripts.
					debugC(kDebugLevelResMan, "[resMan] Attempt to unlock non-existent resource %s", id.toString().c_str());
			}
		}
	}
	return s->r_acc;
}

reg_t kResCheck(EngineState *s, int argc, reg_t *argv) {
	Resource *res = nullptr;
	ResourceType restype = g_sci->getResMan()->convertResType(argv[0].toUint16());

	if ((restype == kResourceTypeAudio36) || (restype == kResourceTypeSync36)) {
		if (argc >= 6) {
			uint noun = argv[2].toUint16() & 0xff;
			uint verb = argv[3].toUint16() & 0xff;
			uint cond = argv[4].toUint16() & 0xff;
			uint seq = argv[5].toUint16() & 0xff;

			res = g_sci->getResMan()->testResource(ResourceId(restype, argv[1].toUint16(), noun, verb, cond, seq));
		}
	} else {
		res = g_sci->getResMan()->testResource(ResourceId(restype, argv[1].toUint16()));
	}

#ifdef ENABLE_SCI32
	// At least LSL6-Hires explicitly treats wave and audio resources the same
	// in its check routine. This was removed in later interpreters. It may be
	// in others, but LSL6 is the only game known to have scripts that rely on
	// this behavior for anything except except kLoad/kUnload calls. Bug #13549
	if (g_sci->getGameId() == GID_LSL6HIRES) {
		if (restype == kResourceTypeWave && res == nullptr) {
			res = g_sci->getResMan()->testResource(ResourceId(kResourceTypeAudio, argv[1].toUint16()));
		}
	}

	// GK2 stores some VMDs inside of resource volumes, but usually videos are
	// streamed from the filesystem.
	if (res == nullptr) {
		const char *format;
		switch (restype) {
		case kResourceTypeRobot:
			format = "%u.rbt";
			break;
		case kResourceTypeDuck:
			format = "%u.duk";
			break;
		case kResourceTypeVMD:
			format = "%u.vmd";
			break;
		default:
			format = nullptr;
		}

		if (format) {
			const Common::Path fileName(Common::String::format(format, argv[1].toUint16()));
			return make_reg(0, Common::File::exists(fileName));
		}
	}
#endif

	return make_reg(0, res != nullptr);
}

reg_t kClone(EngineState *s, int argc, reg_t *argv) {
	reg_t parentAddr = argv[0];
	const Object *parentObj = s->_segMan->getObject(parentAddr);
	reg_t cloneAddr;
	Clone *cloneObj; // same as Object*

	if (!parentObj) {
		error("Attempt to clone non-object/class at %04x:%04x failed", PRINT_REG(parentAddr));
		return NULL_REG;
	}

	debugC(kDebugLevelMemory, "Attempting to clone from %04x:%04x", PRINT_REG(parentAddr));

	uint16 infoSelector = parentObj->getInfoSelector().toUint16();
	cloneObj = s->_segMan->allocateClone(&cloneAddr);

	if (!cloneObj) {
		error("Cloning %04x:%04x failed-- internal error", PRINT_REG(parentAddr));
		return NULL_REG;
	}

	// In case the parent object is a clone itself we need to refresh our
	// pointer to it here. This is because calling allocateClone might
	// invalidate all pointers, references and iterators to data in the clones
	// segment.
	//
	// The reason why it might invalidate those is, that the segment code
	// (Table) uses Common::Array for internal storage. Common::Array now
	// might invalidate references to its contained data, when it has to
	// extend the internal storage size.
	if (infoSelector & kInfoFlagClone)
		parentObj = s->_segMan->getObject(parentAddr);

	*cloneObj = *parentObj;

	// Mark as clone
	infoSelector &= ~kInfoFlagClass; // remove class bit
	cloneObj->setInfoSelector(make_reg(0, infoSelector | kInfoFlagClone));

	cloneObj->setSpeciesSelector(cloneObj->getPos());
	if (parentObj->isClass())
		cloneObj->setSuperClassSelector(parentObj->getPos());
	s->_segMan->getScript(parentObj->getPos().getSegment())->incrementLockers();
	s->_segMan->getScript(cloneObj->getPos().getSegment())->incrementLockers();

	return cloneAddr;
}

reg_t kDisposeClone(EngineState *s, int argc, reg_t *argv) {
	reg_t obj = argv[0];
	Clone *object = s->_segMan->getObject(obj);

	if (!object) {
		error("Attempt to dispose non-class/object at %04x:%04x",
		         PRINT_REG(obj));
		return s->r_acc;
	}

	// SCI uses this technique to find out, if it's a clone and if it's supposed to get freed
	//  At least kq4early relies on this behavior. The scripts clone "Sound", then set bit 1 manually
	//  and call kDisposeClone later. In that case we may not free it, otherwise we will run into issues
	//  later, because kIsObject would then return false and Sound object wouldn't get checked.
	uint16 infoSelector = object->getInfoSelector().toUint16();
	if ((infoSelector & 3) == kInfoFlagClone)
		object->markAsFreed();

	return s->r_acc;
}

// Returns script dispatch address index in the supplied script
reg_t kScriptID(EngineState *s, int argc, reg_t *argv) {
	int script = argv[0].toUint16();
	uint16 index = (argc > 1) ? argv[1].toUint16() : 0;

	if (argv[0].getSegment())
		return argv[0];

	SegmentId scriptSeg = s->_segMan->getScriptSegment(script, SCRIPT_GET_LOAD);

	if (!scriptSeg)
		return NULL_REG;

	Script *scr = s->_segMan->getScript(scriptSeg);

	if (!scr->getExportsNr()) {
		// This is normal. Some scripts don't have a dispatch (exports) table,
		// and this call is probably used to load them in memory, ignoring
		// the return value. If only one argument is passed, this call is done
		// only to load the script in memory. Thus, don't show any warning,
		// as no return value is expected. If an export is requested, then
		// it will most certainly fail with OOB access.
		if (argc == 2)
			error("Script 0x%x does not have a dispatch table and export %d "
					"was requested from it", script, index);
		return NULL_REG;
	}

	// WORKAROUND: Avoid referencing invalid export 0 in script 601 (Snakes & Ladders) in Hoyle 3 Amiga
	if (g_sci->getGameId() == GID_HOYLE3 && g_sci->getPlatform() == Common::kPlatformAmiga && script == 601 && argc == 1)
		return NULL_REG;

	const uint32 address = scr->validateExportFunc(index, true) + scr->getHeapOffset();
	return make_reg32(scriptSeg, address);
}

reg_t kDisposeScript(EngineState *s, int argc, reg_t *argv) {
	int script = argv[0].getOffset();

	SegmentId id = s->_segMan->getScriptSegment(script);
	Script *scr = s->_segMan->getScriptIfLoaded(id);
	if (scr && !scr->isMarkedAsDeleted()) {
		if (s->_executionStack.back().addr.pc.getSegment() != id)
			scr->setLockers(1);
	}

	s->_segMan->uninstantiateScript(script);

	if (argc != 2) {
		return s->r_acc;
	} else {
		return argv[1];
	}
}

reg_t kIsObject(EngineState *s, int argc, reg_t *argv) {
	if (argv[0].getOffset() == SIGNAL_OFFSET) // Treated specially
		return NULL_REG;
	else
		return make_reg(0, s->_segMan->isHeapObject(argv[0]));
}

reg_t kRespondsTo(EngineState *s, int argc, reg_t *argv) {
	reg_t obj = argv[0];
	int selector = argv[1].toUint16();

	return make_reg(0, s->_segMan->isHeapObject(obj) && lookupSelector(s->_segMan, obj, selector, nullptr, nullptr) != kSelectorNone);
}

} // End of namespace Sci
