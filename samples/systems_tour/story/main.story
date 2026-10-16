# The systems tour's one conversation: the guide talks to the player who owns the NPC.
@label start
@ending done
@end

@label guide
Guide: Welcome to the systems tour. #hello
@set talks +1
Guide:
  > "What runs on the server?" -> guide_server
  > "Goodbye" -> guide_bye
@label guide_server
Guide(warm): Everything marked replicated is mirrored to the clients.
@flag asked_server
@return
@label guide_bye
@return
