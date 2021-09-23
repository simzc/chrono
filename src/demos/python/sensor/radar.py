import pychrono.core as chrono
import pychrono.sensor as sens

import numpy as np
import time
import random
import cv2
import math

class simulation:

    def __init__(self) -> None:
        self.system = chrono.ChSystemNSC()
        self.system.Set_G_acc(chrono.ChVectorD(0,0,0))

        green = self.init_vis_mat(chrono.ChVectorF(0,1,0))
        black = self.init_vis_mat(chrono.ChVectorF(1,1,1))
        yellow = self.init_vis_mat(chrono.ChVectorF(1,1,0))

        ground = chrono.ChBodyEasyBox(1000,20,1,1000,True,False)
        ground.SetPos(chrono.ChVectorD(0,0,-1))
        ground.SetBodyFixed(True)
        asset = ground.GetAssets()[0]
        visual_asset = chrono.CastToChVisualization(asset)
        visual_asset.material_list.append(green)
        self.system.Add(ground)

        egocar = chrono.ChBodyEasyBox(5,2,2,1000,True,False)
        egocar.SetPos(chrono.ChVectorD(0,1,1))
        car_asset = egocar.GetAssets()[0]
        car_visual_asset = chrono.CastToChVisualization(car_asset)
        car_visual_asset.material_list.append(yellow)
        self.system.Add(egocar)

        frontcar = chrono.ChBodyEasyBox(5,2,2,1000,True,False)
        frontcar.SetPos(chrono.ChVectorD(10,2,1))
        frontcar.SetPos_dt(chrono.ChVectorD(1,0,0))
        frontcar_asset = frontcar.GetAssets()[0]
        frontcar_visual_asset = chrono.CastToChVisualization(frontcar_asset)
        frontcar_visual_asset.material_list.append(yellow)
        self.system.Add(frontcar)

        # incoming cars on the left lane
        for i in range(10):
            leftcar = chrono.ChBodyEasyBox(5,2,2,1000,True,False)
            leftcar.SetPos(chrono.ChVectorD(10 + i * 10 ,5,1))
            leftcar.SetPos_dt(chrono.ChVectorD(-1,0,0))
            leftcar_asset = frontcar.GetAssets()[0]
            leftcar_visual_asset = chrono.CastToChVisualization(leftcar_asset)
            leftcar_visual_asset.material_list.append(yellow)
            self.system.Add(leftcar)


        # cars in the right lane


        offset_pose = chrono.ChFrameD(chrono.ChVectorD(3,0,0), chrono.Q_from_AngZ(0))
        self.adding_sensors(egocar, offset_pose)


    # color should be a chrono.ChVectorF(float,float,float)
    def init_vis_mat(self, color):
        vis_mat = chrono.ChVisualMaterial()
        vis_mat.SetDiffuseColor(color)
        vis_mat.SetSpecularColor(chrono.ChVectorF(1,1,1))
        return vis_mat

    def adding_sensors(self, body, offset_pose):
        self.manager = sens.ChSensorManager(self.system)
        intensity = 1.0
        self.manager.scene.AddPointLight(chrono.ChVectorF(
            2, 2.5, 100), chrono.ChVectorF(intensity, intensity, intensity), 500.0)
        self.manager.scene.AddPointLight(chrono.ChVectorF(
            9, 2.5, 100), chrono.ChVectorF(intensity, intensity, intensity), 500.0)
        self.manager.scene.AddPointLight(chrono.ChVectorF(
            16, 2.5, 100), chrono.ChVectorF(intensity, intensity, intensity), 500.0)
        self.manager.scene.AddPointLight(chrono.ChVectorF(
            23, 2.5, 100), chrono.ChVectorF(intensity, intensity, intensity), 500.0)
       
        update_rate = 30
        lag = 0
        exposure_time = 0

        self.hfov = math.pi / 3
        self.vfov = math.pi / 9
        hfov = self.hfov
        vfov = self.vfov
        self.image_width = 1280
        image_width = self.image_width
        self.image_height = int(image_width * vfov / hfov)
        image_height = self.image_height

        self.cam = sens.ChCameraSensor(body, update_rate,offset_pose,image_width,image_height,hfov)
        self.cam.SetName("Camera Sensor")
        self.cam.SetLag(lag)
        self.cam.SetCollectionWindow(exposure_time)
        self.cam.PushFilter(sens.ChFilterRGBA8Access())
        self.manager.AddSensor(self.cam)

        h_samples = 300
        v_samples = 100

        self.radar = sens.ChRadarSensor(body,update_rate,offset_pose,h_samples,v_samples,hfov, vfov/2, -vfov/2,50.0)
        self.radar.PushFilter(sens.ChFilterRadarProcess())
        self.radar.PushFilter(sens.ChFilterRadarXYZAccess())
        self.manager.AddSensor(self.radar)


    def sim_advance(self):
        step_size = 1e-3
        self.manager.Update()
        self.system.DoStepDynamics(step_size)
        self.display_image()

    def display_image(self):
        rgba8_buffer = self.cam.GetMostRecentRGBA8Buffer()
        if rgba8_buffer.HasData():
            rgba8_data = rgba8_buffer.GetRGBA8Data()
#            print('RGBA8 buffer recieved from cam. Camera resolution: {0}x{1}'
#                  .format(rgba8_buffer.Width, rgba8_buffer.Height))
#            print('First Pixel: {0}'.format(rgba8_data[0, 0, :]))
            np.flip(rgba8_data)
            bgr = cv2.cvtColor(rgba8_data[::-1], cv2.COLOR_RGB2BGR)
        radar_buffer = self.radar.GetMostRecentRadarXYZBuffer()
        if radar_buffer.HasData():
            radar_data = radar_buffer.GetRadarXYZData()[0]
            
        if rgba8_buffer.HasData():
            if radar_buffer.HasData():
                for i in radar_data:
                    box_x = self.image_width - (int(self.image_width / self.hfov * math.atan2(i[1],i[0])) + int(self.image_width / 2))
                    box_y = self.image_height - (int(self.image_height / self.vfov * math.atan2(i[2],i[0])) + int(self.image_height / 2))
                    intensity = i[6]
                    if abs(i[3]) < 1e-2:
                        # positive relative velocity -> blue
                        cv2.rectangle(bgr,(box_x - 1, box_y - 1), (box_x+1,box_y+1),(0,0,0),int(2))
                    elif i[3] < 0:
                        # negative relative velocity -> red
                        cv2.rectangle(bgr,(box_x - 1, box_y - 1), (box_x+1,box_y+1),(255,0,0),int(2))
                        pass
                    else:
                        # neutral -> white
                        cv2.rectangle(bgr,(box_x - 1, box_y + 1), (box_x+1,box_y-1),color=(0,0,255),thickness=int(2))

            cv2.imshow("window", bgr)
            if cv2.waitKey(1):
                return


def main():
    sim = simulation()
    while True:
        sim.sim_advance()

if __name__ == "__main__":
    main()

