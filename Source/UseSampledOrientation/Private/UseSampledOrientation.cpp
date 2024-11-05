#include "UseSampledOrientation.h"

#include "AbstractInstanceManager.h"
#include "FGBuildGun.h"
#include "FGBuildGunBuild.h"
#include "FGHologram.h"
#include "FGLightweightBuildableSubsystem.h"
#include "Patching/NativeHookManager.h"

DEFINE_LOG_CATEGORY(LogUseSampledOrientation)

#define LOCTEXT_NAMESPACE "FUseSampledOrientationModule"

// Set this to 1 before building to actually log debug messages, 0 to turn them into no-ops at compile time
// I made this compile-time because I'm lazy, don't want to mess with log levels, and would prefer the shipping mod minimize perf impact.
#define LOG_DEBUG_INFO 1

#if LOG_DEBUG_INFO
#define USO_LOG(Verbosity, Format, ...) \
    UE_LOG( LogUseSampledOrientation, Verbosity, Format, ##__VA_ARGS__ )
#else
#define USO_LOG(Verbosity, Format, ...)
#endif

void FUseSampledOrientationModule::StartupModule()
{
    if (WITH_EDITOR)
    {
        USO_LOG(Verbose, TEXT("StartupModule: Not hooking anything because WITH_EDITOR is true!"));
        return;
    }

    USO_LOG(Verbose, TEXT("StartupModule: Hooking functions..."));

    SUBSCRIBE_METHOD(
        UFGBuildGunState::OnRecipeSampled,
        [=](auto& scope, UFGBuildGunState* buildGunState, TSubclassOf<class UFGRecipe> recipe)
        {
            USO_LOG(Verbose, TEXT("UFGBuildGunState::OnRecipeSampled!"));

            auto buildGun = buildGunState->GetBuildGun();
            auto buildState = Cast<UFGBuildGunStateBuild>(buildGun->GetBuildGunStateFor(EBuildGunState::BGS_BUILD));
            if (!buildState)
            {
                USO_LOG(Verbose, TEXT("UFGBuildGunState::OnRecipeSampled. Could not the build state of the build gun??. Build gun state is: %s"), *buildGunState->GetClass()->GetName());
                scope(buildGunState, recipe);
                return;
            }

            auto& hitResult = buildGun->GetHitResult();
            auto actor = hitResult.GetActor();
            if (actor && actor->IsA(AAbstractInstanceManager::StaticClass()))
            {
                if (auto manager = AAbstractInstanceManager::GetInstanceManager(actor))
                {
                    FInstanceHandle handle;
                    if (manager->ResolveHit(hitResult, handle))
                    {
                        actor = handle.GetOwner();
                    }
                }
            }

            if (!actor || Cast<AFGLightweightBuildableSubsystem>(actor) != nullptr )
            {
                // Either we couldn't resolve the actor or it's a lightweight buildable, like a foundation or a wall.
                // We'll just use the default behavior for all of these cases, because trying to align them with the sampled object is weird and usually unnecessary.
                // - Foundations seem to have binary "snap staight or snap diagonal" behavior, instances in the world have inconsistent and changing yaws, and trying
                //   to align automatically according to the game's internal values results in the "snap diagonal" behavior, which is the opposite of what we often want.
                // - Walls must snap to foundations or other walls and do so readily no matter their rotation, so it usually doesn't matter at all and trying to "align"
                //   while snapped doesn't alter them anyway
                USO_LOG(Verbose, TEXT("UFGBuildGunState::OnRecipeSampled. No resolved actor or it's a lightweight buildable, like a foundation or a wall, so using default behavior."));
                scope(buildGunState, recipe);
                return;
            }

            auto desiredYaw = FMath::RoundToInt32(actor->GetActorRotation().Yaw);// Rotation in degrees around the z (vertical) axis
            USO_LOG(Verbose, TEXT("UFGBuildGunState::OnRecipeSampled. Resolved actor is %s with rotation %s. Desired yaw is %d"), *actor->GetClass()->GetName(), *actor->GetActorRotation().ToString(), desiredYaw);

            auto initialHologram = buildState->GetHologram();
            if (!initialHologram)
            {
                USO_LOG(Verbose, TEXT("UFGBuildGunState::OnRecipeSampled. Not in build mode. Will enter build mode upon sample. Simply setting previous scroll rotation to desired yaw %d"), desiredYaw);
                buildState->mPreviousScrollRotation = desiredYaw;
                scope(buildGunState, recipe);
                return;
            }

            scope(buildGunState, recipe);

            auto currentHologram = buildState->GetHologram();
            if (!currentHologram)
            {
                USO_LOG(Verbose, TEXT("UFGBuildGunState::OnRecipeSampled. No hologram in the build gun after sample. This shouldn't happen?"));
                return;
            }

            auto startingYaw = currentHologram->GetScrollRotateValue(); // Absolute yaw orientation in the world if it weren't snapped (but it might be snapped!)
            USO_LOG(Verbose, TEXT("UFGBuildGunState::OnRecipeSampled. Hologram is a %s with yaw %d"), *currentHologram->GetClass()->GetName(), startingYaw);

            auto degreesPerScroll = currentHologram->GetRotationStep();
            if (degreesPerScroll != 0)
            {
                USO_LOG(Verbose, TEXT("UFGBuildGunState::OnRecipeSampled. Hologram rotation step is %d"), degreesPerScroll);
            }
            else
            {
                // If degrees per scroll is zero, it means the function was not implemented for the hologram in it's current state and we
                // have to test and measure it.  We will scroll one time and measure how much that rotates
                buildGun->Scroll(1);
                auto updatedYaw = currentHologram->GetScrollRotateValue();
                degreesPerScroll = updatedYaw - startingYaw;
                if (degreesPerScroll < 0)
                {
                    // if it's negative, we started in the positive and wrapped to the negative, so add 360 to bring it back to positive
                    degreesPerScroll = degreesPerScroll + 360;
                }

                if (degreesPerScroll == 0)
                {
                    USO_LOG(Verbose, TEXT("UFGBuildGunState::OnRecipeSampled. Degrees per scroll is %d! Unscrolling and exiting!"), degreesPerScroll);
                    buildGun->Scroll(-1); // Undo the scroll we just did; it didn't give us any information
                    return;
                }

                startingYaw = updatedYaw;

                USO_LOG(Verbose, TEXT("UFGBuildGunState::OnRecipeSampled. Scrolled once. Hologram starting yaw is now %d and degrees per scroll is %d"), startingYaw, degreesPerScroll);
            }

            auto degreesFromDesiredYaw = desiredYaw - startingYaw;
            auto result = std::div(degreesFromDesiredYaw, degreesPerScroll);
            auto scrollCount = result.quot;
            auto remainder = result.rem;
            if (remainder > degreesPerScroll / 2 )
            {
                scrollCount += 1;
            }
            else if (remainder < -degreesPerScroll / 2)
            {
                scrollCount -= 1;
            }

            USO_LOG(Verbose, TEXT("UFGBuildGunState::OnRecipeSampled. Degrees from desired yaw is %d. Remainder was %d so final scroll count is %d."), degreesFromDesiredYaw, remainder, scrollCount);
            if (scrollCount != 0)
            {
                buildGun->Scroll(scrollCount);
                USO_LOG(Verbose, TEXT("UFGBuildGunState::OnRecipeSampled. Scrolled %d times. Scroll rotate yaw is now: %d."), scrollCount, currentHologram->GetScrollRotateValue());
            }
        });
}

#undef USO_LOG
#undef LOCTEXT_NAMESPACE

IMPLEMENT_MODULE(FUseSampledOrientationModule, UseSampledOrientation)