#pragma once
#include "CoreMinimal.h"
#include "UVLightReactionComponent.h"
#include "UVLightReactionFromOverlap.generated.h"

class AActor;

UCLASS(Blueprintable, ClassGroup = Custom, meta = (BlueprintSpawnableComponent))
class FNAF9_API UUVLightReactionFromOverlap : public UUVLightReactionComponent {
    GENERATED_BODY()
public:
    UUVLightReactionFromOverlap();

protected:
    virtual void BeginPlay() override;

private:
    UFUNCTION(BlueprintCallable)
    void OnOwnerEndOverlap(AActor* OverlappedActor, AActor* OtherActor);

    UFUNCTION(BlueprintCallable)
    void OnOwnerBeginOverlap(AActor* OverlappedActor, AActor* OtherActor);

};