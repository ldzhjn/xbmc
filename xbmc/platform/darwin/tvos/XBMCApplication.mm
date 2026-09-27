/*
 *  Copyright (C) 2010-2018 Team Kodi
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#import "platform/darwin/tvos/XBMCApplication.h"

#import "platform/darwin/NSLogDebugHelpers.h"
#import "platform/darwin/tvos/PreflightHandler.h"
#import "platform/darwin/tvos/TVOSTopShelf.h"
#import "platform/darwin/tvos/XBMCController.h"

#import <AVFoundation/AVFoundation.h>

@implementation XBMCApplicationDelegate

- (void)applicationWillTerminate:(UIApplication*)application
{
  for (UIScene* scene in application.connectedScenes)
  {
    if ([scene.delegate isKindOfClass:[XBMCSceneDelegate class]])
    {
      UIWindow* window = static_cast<XBMCSceneDelegate*>(scene.delegate).window;
      [static_cast<XBMCController*>(window.rootViewController) stopAnimation];
    }
  }
}

- (BOOL)application:(UIApplication*)application
    didFinishLaunchingWithOptions:(NSDictionary*)launchOptions
{
  // check if apple removed our Cache folder first
  // this will trigger the restore if there is a backup available
  CPreflightHandler::CheckForRemovedCacheFolder();

  // This needs to run before anything does any CLog::Log calls
  // as they will directly cause guisetting to get accessed/created
  // via debug log settings.
  CPreflightHandler::MigrateUserdataXMLToNSUserDefaults();

  // audio session setup
  auto audioSession = AVAudioSession.sharedInstance;
  NSError* err = nil;
  if (![audioSession setCategory:AVAudioSessionCategoryPlayback error:&err])
    NSLog(@"audioSession setCategory failed: %@", err);

  err = nil;
  if (![audioSession setMode:AVAudioSessionModeMoviePlayback error:&err])
    NSLog(@"audioSession setMode failed: %@", err);

  err = nil;
  if (![audioSession setActive:YES error:&err])
    NSLog(@"audioSession setActive failed: %@", err);

  return YES;
}

- (BOOL)application:(UIApplication*)app
            openURL:(NSURL*)url
            options:(NSDictionary<NSString*, id>*)options
{
  NSArray* urlComponents = [url.absoluteString componentsSeparatedByString:@"/"];
  NSString* action = urlComponents[2];
  if ([action isEqualToString:@"display"] || [action isEqualToString:@"play"])
    CTVOSTopShelf::GetInstance().HandleTopShelfUrl(url.absoluteString.UTF8String, true);
  return YES;
}
@end

@implementation XBMCSceneDelegate
{
  BOOL _enteredBackground;
}

- (XBMCController*)xbmcController
{
  return static_cast<XBMCController*>(self.window.rootViewController);
}

- (void)scene:(UIScene*)scene
    willConnectToSession:(UISceneSession*)session
               options:(UISceneConnectionOptions*)connectionOptions
{
  if (![scene isKindOfClass:[UIWindowScene class]])
    return;

  self.window = [[UIWindow alloc] initWithWindowScene:static_cast<UIWindowScene*>(scene)];
  self.window.rootViewController = [XBMCController new];
  [self.window makeKeyAndVisible];
  [self.xbmcController startAnimation];

  [self scene:scene openURLContexts:connectionOptions.URLContexts];
}

- (void)sceneDidEnterBackground:(UIScene*)scene
{
  _enteredBackground = YES;
  [self.xbmcController pauseAnimation];
  [self.xbmcController enterBackground];
}

- (void)sceneWillEnterForeground:(UIScene*)scene
{
  if (_enteredBackground)
  {
    _enteredBackground = NO;
    [self.xbmcController resumeAnimation];
    [self.xbmcController enterForeground];
  }
}

- (void)scene:(UIScene*)scene openURLContexts:(NSSet<UIOpenURLContext*>*)URLContexts
{
  for (UIOpenURLContext* context in URLContexts)
  {
    NSArray* urlComponents = [context.URL.absoluteString componentsSeparatedByString:@"/"];
    if (urlComponents.count < 3)
      continue;
    NSString* action = urlComponents[2];
    if ([action isEqualToString:@"display"] || [action isEqualToString:@"play"])
      CTVOSTopShelf::GetInstance().HandleTopShelfUrl(context.URL.absoluteString.UTF8String, true);
  }
}

@end

static void SigPipeHandler(int s)
{
  NSLog(@"We Got a Pipe Signal: %d____________", s);
}

int main(int argc, char* argv[])
{
  @autoreleasepool
  {
    signal(SIGPIPE, SigPipeHandler);

    int retVal = 0;
    @try
    {
      retVal =
          UIApplicationMain(argc, argv, nil, NSStringFromClass([XBMCApplicationDelegate class]));
    }
    @catch (id theException)
    {
      ELOG(@"%@", theException);
    }
    @finally
    {
      ILOG(@"This always happens.");
    }

    return retVal;
  }
}
