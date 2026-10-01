import { ComponentFixture, TestBed } from '@angular/core/testing';

import { CallHomeNotificationComponent } from './call-home-notification.component';
import { HttpClientTestingModule } from '@angular/common/http/testing';
import { RouterTestingModule } from '@angular/router/testing';
import { configureTestBed } from '~/testing/unit-test-helper';
import { SharedModule } from '../../shared.module';

describe('CallHomeNotificationComponent', () => {
  let component: CallHomeNotificationComponent;
  let fixture: ComponentFixture<CallHomeNotificationComponent>;

  configureTestBed({
    declarations: [CallHomeNotificationComponent],
    imports: [HttpClientTestingModule, RouterTestingModule, SharedModule]
  });

  beforeEach(() => {
    fixture = TestBed.createComponent(CallHomeNotificationComponent);
    component = fixture.componentInstance;
    fixture.detectChanges();
  });

  it('should create', () => {
    expect(component).toBeTruthy();
  });
});
