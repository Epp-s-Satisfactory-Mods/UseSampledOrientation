#include "UseSampledOrientation.h"

#include "AbstractInstanceManager.h"
#include "FGBuildableConveyorBelt.h"
#include "FGBuildablePipeBase.h"
#include "FGBuildableRailroadTrack.h"
#include "FGBuildGun.h"
#include "FGBuildGunBuild.h"
#include "FGConveyorBeltHologram.h"
#include "FGConveyorPoleHologram.h"
#include "FGHologram.h"
#include "FGLightweightBuildableSubsystem.h"
#include "FGPipelineHologram.h"
#include "FGPipelineSupportHologram.h"
#include "FGSplineHologram.h"
#include "FGWallAttachmentHologram.h"
#include "Patching/NativeHookManager.h"

DEFINE_LOG_CATEGORY(LogUseSampledOrientation)

// The mod template does this but we have no text to localize
#define LOCTEXT_NAMESPACE "FUseSampledOrientationModule"

// When we're building to ship, set this to 0 to no-op logging and minimize performance impact. Would prefer to do this through
// build defines based on whether we're building for development or shipping but at the moment alpakit always builds shipping.
#define USO_LOG_DEBUG_TEXT 1

#if USO_LOG_DEBUG_TEXT
#define USO_LOG(Verbosity, Format, ...)\
    UE_LOG( LogUseSampledOrientation, Verbosity, Format, ##__VA_ARGS__ )
#else
#define USO_LOG(Verbosity, Format, ...)
#endif


void SetHologramRotationFromTransform(AFGHologram* hologram, FTransform desiredTransform)
{
    USO_LOG(Verbose, TEXT("SetHologramRotationFromTransform. Hologram is a %s with rotation step %d"), *hologram->GetClass()->GetName(), hologram->GetRotationStep());

    USO_LOG(Verbose, TEXT("SetHologramRotationFromTransform. 1 Hologram has rotation %s, rotate value %d"),
        *hologram->GetActorRotation().ToString(),
        hologram->GetScrollRotateValue());

    hologram->SetActorTransform(desiredTransform);

    USO_LOG(Verbose, TEXT("SetHologramRotationFromTransform. 2 Hologram has rotation %s, rotate value %d"),
        *hologram->GetActorRotation().ToString(),
        hologram->GetScrollRotateValue());

    hologram->UpdateRotationValuesFromTransform();

    USO_LOG(Verbose, TEXT("SetHologramRotationFromTransform. 3 Hologram has rotation %s, rotate value %d"),
        *hologram->GetActorRotation().ToString(),
        hologram->GetScrollRotateValue());
}

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
        [](auto& scope, UFGBuildGunState* buildGunState, TSubclassOf<class UFGRecipe> recipe)
        {
            // Resolve the actor at the hit result
            auto buildGun = buildGunState->GetBuildGun();
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

            USO_LOG(Verbose, TEXT("UFGBuildGunState::OnRecipeSampled. Actor is %s with transform %s."), *actor->GetClass()->GetName(), *actor->GetTransform().ToHumanReadableString());

            scope(buildGunState, recipe);

            if (!actor || Cast<AFGLightweightBuildableSubsystem>(actor) != nullptr)
            {
                // Either we couldn't resolve the actor or it's a lightweight buildable (like a foundation or a wall).
                // We'll just use the default behavior for these cases, because trying to align them with the sampled object is weird and usually unnecessary.
                // - Foundations seem to have binary "snap staight or snap diagonal" behavior, instances in the world have inconsistent and changing yaws, and trying
                //   to align automatically according to the game's internal values results in the "snap diagonal" behavior, which is the opposite of what we often want.
                // - Walls must snap to foundations or other walls and do so readily no matter their rotation, so it usually doesn't matter at all and trying to "align"
                //   while snapped doesn't alter them anyway.
                USO_LOG(Verbose, TEXT("UFGBuildGunState::OnRecipeSampled. Actor is %s, which is a special case we can't handle. Just using default behavior."), *actor->GetClass()->GetName());
                return;
            }

            auto buildState = Cast<UFGBuildGunStateBuild>(buildGun->GetBuildGunStateFor(EBuildGunState::BGS_BUILD));
            if (!buildState)
            {
                USO_LOG(Verbose, TEXT("UFGBuildGunState::OnRecipeSampled. Could not get the build state of the build gun??. Build gun state is: %s"), *buildGunState->GetClass()->GetName());
                return;
            }

            auto hologram = buildState->GetHologram();
            if (!hologram)
            {
                USO_LOG(Verbose, TEXT("UFGBuildGunState::OnRecipeSampled. No hologram in the build gun after sample. This shouldn't happen?"));
                return;
            }

            if (auto splineHologram = Cast<AFGSplineHologram>(hologram))
            {
                if (splineHologram->mBuildStep != ESplineHologramBuildStep::SHBS_FindStart)
                {
                    // If we have advanced the build step in a spline hologram at all and sampled the same kind of spline, we will be here. We short-circuit because:
                    //  1) Attempting to set the rotation of a supporting pole hologram that has already been anchored will turn it invisible and if the
                    //     player finishes the construction, it will be an invisible, permanent actor that can't be fixed (as far as I can tell).
                    //  2) That's what the game does, anyway - sampling the same kind of spline with your current spline in a non-starting build step is a no-op
                    USO_LOG(Verbose, TEXT("UFGBuildGunState::OnRecipeSampled. Spline hologram is being built. Current state is %d so we're defaulting to no-op."), splineHologram->mBuildStep);
                    return;
                }
            }

            // If there are no special cases, we'll set the rotation of the hologram using the default actor transform
            FTransform desiredTransform = actor->GetActorTransform();
            USO_LOG(Verbose, TEXT("UFGBuildGunState::OnRecipeSampled. Base actor transform is: %s"), *desiredTransform.ToHumanReadableString());

            // The apis for these spline-based holograms all have similar APIs but they don't share a common type and have slightly different
            // behavior, so we handle each case individually

            // Conveyor belts (but not lifts!)
            if (auto conveyorBelt = Cast<AFGBuildableConveyorBelt>(actor))
            {
                USO_LOG(Verbose, TEXT("UFGBuildGunState::OnRecipeSampled.\tConveyor belt"));

                auto conveyorBeltHologram = Cast<AFGConveyorBeltHologram>(hologram);
                if (!conveyorBeltHologram)
                {
                    USO_LOG(Verbose, TEXT("UFGBuildGunState::OnRecipeSampled.\tSampled a conveyor belt but hologram wasn't a conveyor belt?"));
                    return;
                }

                auto hitResultOffset = conveyorBelt->FindOffsetClosestToLocation(hitResult.Location);
                USO_LOG(Verbose, TEXT("UFGBuildGunState::OnRecipeSampled.\t Offset: %f "), hitResultOffset);
                FVector outLocation;
                FVector outDirection;
                conveyorBelt->GetLocationAndDirectionAtOffset(hitResultOffset, outLocation, outDirection);
                USO_LOG(Verbose, TEXT("UFGBuildGunState::OnRecipeSampled.\t Desired direction is: %s, which has a rotation of %s"), *outDirection.ToString(), *outDirection.Rotation().ToString());

                // The conveyor pole holograms seem to use an inverted direction from the sampled belts, so just multiply that direction vector by -1 before creating the rotation
                auto conveyorPoleTransform = FTransform(
                    (outDirection * -1).Rotation().Quaternion(),
                    desiredTransform.GetTranslation(),
                    desiredTransform.GetScale3D());

                USO_LOG(Verbose, TEXT("UFGBuildGunState::OnRecipeSampled.\t Conveyor pole transform is: %s"), *conveyorPoleTransform.ToHumanReadableString());

                // Set the rotation of the starting normal pole hologram, off of which the actual conveyor is modeled for ground conveyors
                SetHologramRotationFromTransform(conveyorBeltHologram->mChildPoleHologram[0], conveyorPoleTransform);

                // There are also ceiling pole holograms and wall pole holograms but we let them have the default behavior because:
                //  1) Ceiling poles adjust their rotation based on the rotation of the foundation the player is looking at,
                //     making it infeasible to reliably get them aligned with the originating conveyor.
                //  2) Wall poles are binary but my tests have been confusing - they seem to set the rotation value in
                //     increments of 10 but flip each time and I haven't seen a clear correlation between the desired
                //     transform and which orientation they start in.
                return;
            }

            // Pipelines and hypertubes
            if (auto buildablePipeBase = Cast<AFGBuildablePipeBase>(actor))
            {
                USO_LOG(Verbose, TEXT("UFGBuildGunState::OnRecipeSampled.\tBuildable pipe base"));

                // Both hypertubes and pipelines seem to use this hologram
                auto buildablePipeHologram = Cast<AFGPipelineHologram>(hologram);
                if (!buildablePipeHologram)
                {
                    USO_LOG(Verbose, TEXT("UFGBuildGunState::OnRecipeSampled.\tSampled a pipe but hologram wasn't a pipe?"));
                    return;
                }

                auto hitResultOffset = buildablePipeBase->FindOffsetClosestToLocation(hitResult.Location);
                USO_LOG(Verbose, TEXT("UFGBuildGunState::OnRecipeSampled.\t Offset: %f "), hitResultOffset);
                FVector outLocation;
                FVector outDirection;
                buildablePipeBase->GetLocationAndDirectionAtOffset(hitResultOffset, outLocation, outDirection);
                USO_LOG(Verbose, TEXT("UFGBuildGunState::OnRecipeSampled.\t Desired direction is: %s, which has a rotation of %s"), *outDirection.ToString(), *outDirection.Rotation().ToString());
                desiredTransform.SetRotation(outDirection.Rotation().Quaternion());
                USO_LOG(Verbose, TEXT("UFGBuildGunState::OnRecipeSampled.\t New desired transform is: %s "), *desiredTransform.ToHumanReadableString());

                // Set the rotation of the starting normal pole hologram, off of which the actual pipe is modeled for ground pipes
                SetHologramRotationFromTransform(buildablePipeHologram->mChildPoleHologram[0], desiredTransform);

                // There are also ceiling pole holograms and wall pole holograms but we let them have the default behavior because:
                //  1) Ceiling poles adjust their rotation based on the rotation of the foundation the player is looking at,
                //     making it infeasible to reliably get them aligned with the originating pipeline.
                //  2) Wall pipe tests have been confusing and setting the desired transform just doesn't seem to work.
                return;
            }

            // Railroads. They don't have any child holograms to worry about, but we do need to set the desired transform based on the tangent at
            // the curve that was targeted
            if (auto buildableRailroad = Cast<AFGBuildableRailroadTrack>(actor))
            {
                USO_LOG(Verbose, TEXT("UFGBuildGunState::OnRecipeSampled.\tRailroad"));
                auto hitResultPosition = buildableRailroad->FindTrackPositionClosestToWorldLocation(hitResult.Location);
                USO_LOG(Verbose, TEXT("UFGBuildGunState::OnRecipeSampled.\t Offset: %f "), hitResultPosition.Offset);
                FVector outLocation;
                FVector outDirection;
                buildableRailroad->GetWorldLocationAndDirectionAtPosition(hitResultPosition, outLocation, outDirection);
                USO_LOG(Verbose, TEXT("UFGBuildGunState::OnRecipeSampled.\t Desired direction is: %s, which has a rotation of %s"), *outDirection.ToString(), *outDirection.Rotation().ToString());
                desiredTransform.SetRotation(outDirection.Rotation().Quaternion());
                USO_LOG(Verbose, TEXT("UFGBuildGunState::OnRecipeSampled.\t New desired transform is: %s "), *desiredTransform.ToHumanReadableString());
            }

            SetHologramRotationFromTransform(hologram, desiredTransform);
        });
}

#undef USO_LOG
#undef USO_LOG_DEBUG_TEXT
#undef LOCTEXT_NAMESPACE

IMPLEMENT_MODULE(FUseSampledOrientationModule, UseSampledOrientation)